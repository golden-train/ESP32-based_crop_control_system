// ESP32 传感器数据监控系统
// 从 COM6 串口读取环境温度、湿度、土壤湿度、光照强度
// 未连接开发板时自动切换为模拟演示模式
//
// 多线程架构：
//   主线程    —— SFML 事件循环 + 渲染仪表盘
//   数据线程  —— 定频发送 "0" 触发采集，处理正常数据
//   读取线程  —— 持续监听串口，区分正常响应 / 自发错误上报
//   错误线程  —— 处理 ESP32 自发上传的错误数据
#include "serial_reader.hpp"
#include "serial_dispatcher.hpp"
#include "number_parser.hpp"
#include "sensor_data.hpp"
#include "simulated_sensor.hpp"
#include "data_logger.hpp"
#include "analysis_rules.hpp"
#include "sensor_display.hpp"
#include <windows.h>
#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <queue>
#include <mutex>
#include <condition_variable>

// 采样间隔（秒）：每隔此时间发一次 "0" 触发 ESP32 采集
// ★ 必须与 ESP32 端 HEARTBEAT_MS 保持一致 ★
constexpr int SAMPLE_INTERVAL_SEC = 1;

// 错误数据队列（错误处理线程从中取数据）
static std::queue<std::string> g_errorQueue;
static std::mutex g_mutex;
static std::condition_variable g_errorCv;
static std::atomic<bool> g_running{true};

// 日志文件路径（相对于工作目录）
static DataLogger *g_logger = nullptr;

// SFML 显示指针（由主线程设置，数据线程只读）
static SensorDisplay *g_display = nullptr;

// 输出一行到控制台 + SFML 日志区
static void logMessage(const std::string &msg)
{
    std::cout << msg << std::endl;
    if (g_display)
        g_display->addLog(msg);
}

// 将一行传感器数值填入 SensorData 并打印，同时写入日志和显示
static void processData(const SensorData &data)
{
    std::cout << u8"[传感器] "
              << u8"温度: " << data.temperature << u8"℃  "
              << u8"湿度: " << data.humidity << u8"%  "
              << u8"土壤: " << data.soilMoisture << u8"%  "
              << u8"光照: " << data.lightIntensity << u8"lux"
              << std::endl;

    if (g_logger)
        g_logger->log(data.temperature, data.humidity,
                      data.soilMoisture, data.lightIntensity);

    if (g_display)
        g_display->updateData(data);
}

// 尝试从一行文本解析传感器数据，成功返回 true
static bool tryParseSensorData(const std::string &line, SensorData &outData)
{
    const std::vector<double> values = extractNumbers(line);
    if (values.size() >= 4)
    {
        outData.temperature    = values[0];
        outData.humidity       = values[1];
        outData.soilMoisture   = values[2];
        outData.lightIntensity = values[3];
        return true;
    }
    return false;
}

// 从日志文件读取最新一行，提取最后 4 个数值（温度、湿度、土壤、光照）
static std::vector<double> readLatestFromLog(const std::string &logPath)
{
    std::ifstream file(logPath);
    if (!file.is_open()) return {};

    std::string line, lastLine;
    while (std::getline(file, line))
        if (!line.empty()) lastLine = line;

    if (lastLine.empty()) return {};

    std::vector<double> all = extractNumbers(lastLine);
    if (all.size() < 4) return {};

    return {all[all.size() - 4], all[all.size() - 3],
            all[all.size() - 2], all[all.size() - 1]};
}

// 错误数据处理线程：从错误队列中取出数据并处理
static void errorHandlerThread()
{
    while (g_running)
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        g_errorCv.wait_for(lock, std::chrono::milliseconds(500), [] {
            return !g_errorQueue.empty() || !g_running;
        });

        while (!g_errorQueue.empty())
        {
            std::string line = std::move(g_errorQueue.front());
            g_errorQueue.pop();
            lock.unlock();

            SensorData data;
            if (tryParseSensorData(line, data))
            {
                std::cout << u8"[异常数据] "
                          << u8"温度: " << data.temperature << u8"℃  "
                          << u8"湿度: " << data.humidity << u8"%  "
                          << u8"土壤: " << data.soilMoisture << u8"%  "
                          << u8"光照: " << data.lightIntensity << u8"lux"
                          << std::endl;

                if (g_logger)
                    g_logger->log(data.temperature, data.humidity,
                                  data.soilMoisture, data.lightIntensity);
            }
            else
            {
                logMessage(std::string(u8"[异常上报] ") + line);
            }

            lock.lock();
        }
    }
}

// 真实串口模式（运行在数据线程中）
static void runRealMode(SerialReader &reader)
{
    logMessage(std::string(u8"【实时模式】串口 COM6 已打开，采样间隔 ") +
               std::to_string(SAMPLE_INTERVAL_SEC) + u8" 秒");

    if (g_display) g_display->setConnected(true);

    DataLogger logger("temp/sensor_log.txt");
    g_logger = &logger;

    // 启动错误处理线程
    std::thread errorThread(errorHandlerThread);

    // 启动调度器：后台持续读取串口，自发数据推送到错误队列
    SerialDispatcher dispatcher;
    dispatcher.start(reader.nativeHandle(), [](const std::string &line) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_errorQueue.push(line);
        }
        g_errorCv.notify_one();
    });

    int timeoutCount = 0;
    const int TIMEOUT_WARN_INTERVAL = 5;

    while (g_running)
    {
        dispatcher.expectResponse();

        bool writeErr = false;
        if (!reader.writeText("0", writeErr) || writeErr)
        {
            dispatcher.clearExpectResponse();
            logMessage(u8"[错误] 发送触发指令失败");
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        // waitResponse 有 3 秒超时，防止 ESP32 无响应时永久阻塞
        std::string line = dispatcher.waitResponse(3000);

        if (!g_running) break;
        if (line.empty())
        {
            timeoutCount++;
            if (timeoutCount == 1 || timeoutCount % TIMEOUT_WARN_INTERVAL == 0)
                logMessage(std::string(u8"[警告] 串口无响应，已连续超时 ") +
                           std::to_string(timeoutCount) + u8" 次");
            continue;
        }
        timeoutCount = 0; // 成功收到数据，重置超时计数

        SensorData data;
        if (tryParseSensorData(line, data))
        {
            processData(data);

            std::vector<double> fileValues = readLatestFromLog("temp/sensor_log.txt");
            if (!fileValues.empty())
            {
                AnalysisRules rules;
                std::vector<char> cmds = rules.observe(fileValues);

                static bool prevAnomaly = false;

                if (cmds.empty())
                {
                    if (prevAnomaly)
                    {
                        bool err = false;
                        reader.writeText("9", err);
                        logMessage(u8"[指令] 9 → 关闭所有执行器");
                        prevAnomaly = false;
                    }
                }
                else
                {
                    prevAnomaly = true;
                    for (char cmd : cmds)
                    {
                        const char *label =
                            cmd == '1' ? u8"光照" :
                            cmd == '2' ? u8"湿度" :
                            cmd == '3' ? u8"温度" : u8"土壤";
                        bool err = false;
                        reader.writeText(std::string(1, cmd), err);
                        logMessage(std::string(u8"[指令] ") + cmd +
                                   u8" → " + label + u8"异常处理");
                    }
                }
            }
        }
        else
        {
            std::cout << u8"[警告] 该行仅解析到 "
                      << extractNumbers(line).size() << u8" 个数值: "
                      << line << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::seconds(SAMPLE_INTERVAL_SEC));

        // 检查手动指令队列
        char manualCmd;
        while (SensorDisplay::popCommand(manualCmd))
        {
            bool err = false;
            reader.writeText(std::string(1, manualCmd), err);
            logMessage(std::string(u8"[手动] 发送指令 ") + manualCmd);
        }
    }

    dispatcher.stop();
    g_running = false;
    g_errorCv.notify_all();
    if (errorThread.joinable())
        errorThread.join();
}

// 模拟演示模式（运行在数据线程中）
static void runDemoMode()
{
    logMessage(u8"【演示模式】串口 COM6 不可用，使用模拟数据");
    logMessage(std::string(u8"采样间隔 ") +
               std::to_string(SAMPLE_INTERVAL_SEC) + u8" 秒，关闭窗口退出");

    if (g_display) g_display->setConnected(false);

    DataLogger logger("temp/sensor_log.txt");
    g_logger = &logger;

    std::thread errorThread(errorHandlerThread);

    while (g_running)
    {
        std::cout << u8"[发送] 0" << std::endl;

        SensorData data = SimulatedSensor::generate();
        processData(data);

        {
            std::vector<double> fileValues = readLatestFromLog("temp/sensor_log.txt");
            if (!fileValues.empty())
            {
                AnalysisRules rules;
                std::vector<char> cmds = rules.observe(fileValues);

                static bool prevAnomaly = false;

                if (cmds.empty())
                {
                    if (prevAnomaly)
                    {
                        logMessage(u8"[指令] 9 → 关闭所有执行器");
                        prevAnomaly = false;
                    }
                }
                else
                {
                    prevAnomaly = true;
                    for (char cmd : cmds)
                    {
                        const char *label =
                            cmd == '1' ? u8"光照" :
                            cmd == '2' ? u8"湿度" :
                            cmd == '3' ? u8"温度" : u8"土壤";
                        logMessage(std::string(u8"[指令] ") + cmd +
                                   u8" → " + label + u8"异常处理");
                    }
                }
            }
        }

        static int counter = 0;
        if (++counter % 5 == 0)
        {
            std::string fakeError = "ERR_TEMP_HIGH 38.5 60.0 42.0 800";
            logMessage(std::string(u8"[模拟异常上报] ") + fakeError);
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_errorQueue.push(fakeError);
            }
            g_errorCv.notify_one();
        }

        std::this_thread::sleep_for(std::chrono::seconds(SAMPLE_INTERVAL_SEC));

        // 检查手动指令队列
        char manualCmd;
        while (SensorDisplay::popCommand(manualCmd))
        {
            logMessage(std::string(u8"[手动] 指令 ") + manualCmd + u8"（演示模式）");
        }
    }

    g_running = false;
    g_errorCv.notify_all();
    if (errorThread.joinable())
        errorThread.join();
}

int main()
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    CreateDirectoryA("temp", nullptr);
    std::ofstream("temp/sensor_log.txt", std::ios::trunc).close();

    char cwdBuf[256];
    if (GetCurrentDirectoryA(sizeof(cwdBuf), cwdBuf))
        std::cout << u8"[工作目录] " << cwdBuf << std::endl;

    SensorDisplay display;
    g_display = &display;

    SerialReader reader;
    bool realMode = reader.open("COM6", 115200);

    std::thread dataThread([realMode, &reader]() {
        if (realMode)
            runRealMode(reader);
        else
            runDemoMode();
    });

    while (display.isOpen() && g_running)
    {
        display.processEvents();
        display.render();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    g_running = false;
    if (dataThread.joinable())
        dataThread.join();

    reader.close();
    return 0;
}
