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

static DataLogger *g_logger = nullptr;

// 上次写入日志的时间（用于5分钟间隔判断）
static auto g_lastLogTime = std::chrono::system_clock::time_point{};

// SFML 显示指针（由主线程设置，数据线程只读）
static SensorDisplay *g_display = nullptr;

// 共享分析规则实例（数据线程内持久）
static AnalysisRules *g_rules = nullptr;

// 输出一行到控制台 + SFML 日志区
static void logMessage(const std::string &msg)
{
    std::cout << msg << std::endl;
    if (g_display)
        g_display->addLog(msg);
}

// 将一行传感器数值填入 SensorData 并打印，更新 SFML 仪表盘
static void processData(const SensorData &data)
{
    std::cout << u8"[传感器] "
              << u8"温度: " << data.temperature << u8"℃  "
              << u8"湿度: " << data.humidity << u8"%  "
              << u8"土壤: " << data.soilMoisture << u8"%  "
              << u8"光照: " << data.lightIntensity << u8"lux"
              << std::endl;

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

// 将指令字符转为中文标签
static const char *cmdLabel(char cmd)
{
    switch (cmd)
    {
    case '1': return u8"开补光";
    case '3': return u8"蜂鸣器";
    case '4': return u8"开灌溉";
    case '5': return u8"关补光";
    case '6': return u8"关灌溉";
    case '8': return u8"清除报警";
    case '9': return u8"关闭所有";
    default:  return u8"未知";
    }
}

// 5 分钟分析点：存入历史 + 执行分析 + 发送指令
static void runAnalysisPoint(SensorData &data, SerialReader *reader)
{
    if (!g_rules) return;

    // 0. 同步 UI 配置到分析引擎（种植面积等）
    if (g_display)
    {
        AnalysisConfig cfg = g_rules->getConfig();
        cfg.growingArea = g_display->getGrowingArea();
        g_rules->updateConfig(cfg);
    }

    // 1. 存入一个数据点
    g_rules->feedDataPoint(data);

    // 2. 执行分析
    std::vector<char> cmds = g_rules->checkUrgent();

    // 3. 发送指令 + 同步执行器状态到 UI
    for (char cmd : cmds)
    {
        if (reader)
        {
            bool err = false;
            reader->writeText(std::string(1, cmd), err);
        }
        if (g_display)
            g_display->applyCommand(cmd);
        logMessage(std::string(u8"[分析] 指令 ") + cmd + u8" → " + cmdLabel(cmd));
    }

    // 4. 写日志
    auto _now = std::chrono::system_clock::now();
    if (g_logger && _now - g_lastLogTime >= std::chrono::minutes(5))
    {
        g_logger->log(data.temperature, data.humidity,
                      data.soilMoisture, data.lightIntensity);
        g_lastLogTime = _now;
    }

    // 5. 推送分析数据到 UI 显示
    if (g_display)
    {
        g_display->setAnalysisData(
            g_rules->totalWaterUsed(),
            g_rules->tempAlarmActive(),
            g_rules->avgPpfd(),
            g_rules->currentDli(),
            g_rules->avgTemperature(),
            g_rules->isIrrigating(),
            g_rules->dataPointCount());
    }

    // 6. 输出分析摘要
    logMessage(std::string(u8"[分析] 数据点 #") +
               std::to_string(g_rules->dataPointCount()) +
               u8" | 温度趋势: " + (g_rules->isTrendAlarm() ? u8"↑报警" : u8"正常") +
               u8" | 平均温度: " + std::to_string(g_rules->avgTemperature()).substr(0, 4) + u8"℃" +
               u8" | 用水量: " + std::to_string(g_rules->totalWaterUsed()).substr(0, 4) + u8"L" +
               (g_rules->tempAlarmActive() ? u8" | ⚠ 温度报警" : u8""));
}

// 检查手动指令队列（含 '8' 特殊处理）
static void processManualCommands(SerialReader *reader)
{
    char manualCmd;
    while (SensorDisplay::popCommand(manualCmd))
    {
        if (manualCmd == '8' && g_rules)
        {
            g_rules->clearAlarm();
            logMessage(u8"[手动] 清除温度报警（指令 8）");
        }

        if (g_display)
            g_display->applyCommand(manualCmd);

        if (reader)
        {
            bool err = false;
            reader->writeText(std::string(1, manualCmd), err);
            logMessage(std::string(u8"[手动] 发送指令 ") + manualCmd +
                       u8" → " + cmdLabel(manualCmd));
        }
        else
        {
            logMessage(std::string(u8"[手动] 指令 ") + manualCmd +
                       u8" → " + cmdLabel(manualCmd) + u8"（演示模式）");
        }
    }
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
                    g_logger->logAnomaly(data.temperature, data.humidity,
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

    AnalysisRules rules;
    g_rules = &rules;

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
        timeoutCount = 0;

        SensorData data;
        if (tryParseSensorData(line, data))
        {
            processData(data);

            // 每 5 分钟：分析点（feed + checkUrgent + 日志）
            auto _now = std::chrono::system_clock::now();
            if (_now - g_lastLogTime >= std::chrono::minutes(5))
                runAnalysisPoint(data, &reader);
        }
        else
        {
            std::cout << u8"[警告] 该行仅解析到 "
                      << extractNumbers(line).size() << u8" 个数值: "
                      << line << std::endl;
        }

        // 条件休眠
        if (g_display && g_display->isOpen())
            std::this_thread::sleep_for(std::chrono::seconds(SAMPLE_INTERVAL_SEC));
        else
            std::this_thread::sleep_for(std::chrono::minutes(5));

        // 检查手动指令队列
        processManualCommands(&reader);
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

    AnalysisRules rules;
    g_rules = &rules;

    std::thread errorThread(errorHandlerThread);

    while (g_running)
    {
        // 演示模式：生成模拟数据
        SensorData data = SimulatedSensor::generate();
        processData(data);

        // 每 5 分钟：分析点
        auto _now = std::chrono::system_clock::now();
        if (_now - g_lastLogTime >= std::chrono::minutes(5))
            runAnalysisPoint(data, nullptr); // nullptr = 无串口，仅日志

        // 模拟异常上报（每 5 次）
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

        // 条件休眠
        if (g_display && g_display->isOpen())
            std::this_thread::sleep_for(std::chrono::seconds(SAMPLE_INTERVAL_SEC));
        else
            std::this_thread::sleep_for(std::chrono::minutes(5));

        // 检查手动指令队列
        processManualCommands(nullptr);
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
