// ESP32 传感器数据监控系统
// 数据来源：服务端（首选）→ MQTT → 串口 COM6 → 退出
//
// 多线程架构：
//   主线程    —— SFML 事件循环 + 渲染仪表盘
//   数据线程  —— 服务端/MQTT/串口数据接收 + 分析处理
//   读取线程  —— 持续监听串口，区分正常响应 / 自发错误上报
//   错误线程  —— 处理 ESP32 自发上传的错误数据
#include "serial_reader.hpp"
#include "serial_dispatcher.hpp"
#include "mqtt_client.hpp"
#include "server_client.hpp"
#include "number_parser.hpp"
#include "sensor_data.hpp"
#include "data_logger.hpp"
#include "analysis_rules.hpp"
#include "sensor_display.hpp"
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <sstream>

// Winsock 初始化（全局执行一次）
static struct WsaInit { WsaInit() { WSADATA d; WSAStartup(MAKEWORD(2,2), &d); } } s_wsa;

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

// MQTT 客户端指针（数据线程内持久）
static MqttClient *g_mqtt = nullptr;

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

// 从服务端日志行解析传感器数据
// 格式: "[2026-05-14 10:00:00] 25.3 55.1 42.0 813"
// 格式: "[2026-05-14 10:00:03][ANOMALY] 25.3 55.1 42.0 813"
static bool parseLogLine(const std::string &line, SensorData &outData)
{
    size_t bracketEnd = line.rfind(']');
    if (bracketEnd == std::string::npos)
        return tryParseSensorData(line, outData);
    std::string rest = line.substr(bracketEnd + 1);
    return tryParseSensorData(rest, outData);
}

// 生成 ISO 格式时间字符串（用于服务端日志查询）
static std::string toIsoString(const std::chrono::system_clock::time_point &tp)
{
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm;
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
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
        else if (g_mqtt && g_mqtt->isRunning())
        {
            g_mqtt->publish("esp32/cmd", std::string(1, cmd));
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
        else if (g_mqtt && g_mqtt->isRunning())
        {
            g_mqtt->publish("esp32/cmd", std::string(1, manualCmd));
            logMessage(std::string(u8"[手动] MQTT指令 ") + manualCmd +
                       u8" → " + cmdLabel(manualCmd));
        }
        else
        {
            logMessage(std::string(u8"[手动] 指令 ") + manualCmd +
                       u8" → " + cmdLabel(manualCmd) + u8"（无连接）");
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

// MQTT 数据接收模式（运行在数据线程中）
static void runMqttMode()
{
    logMessage(u8"【MQTT 模式】连接到 8.137.48.225");

    if (g_display) g_display->setConnected(true);

    DataLogger logger("temp/sensor_log.txt");
    g_logger = &logger;

    AnalysisRules rules;
    g_rules = &rules;

    MqttClient mqtt;
    g_mqtt = &mqtt;

    if (!mqtt.begin("8.137.48.225", 1883, "pc", "20061001"))
    {
        logMessage(u8"[错误] MQTT 连接失败");
        g_mqtt = nullptr;
        return;
    }

    std::thread errorThread(errorHandlerThread);

    mqtt.setMessageCallback([&](const std::string &topic, const std::string &payload) {
        if (topic == "esp32/data")
        {
            SensorData data;
            if (tryParseSensorData(payload, data))
            {
                processData(data);
                auto _now = std::chrono::system_clock::now();
                if (_now - g_lastLogTime >= std::chrono::minutes(5))
                    runAnalysisPoint(data, nullptr);
            }
        }
        else if (topic == "esp32/alert")
        {
            std::string cleanPayload = payload;
            if (!cleanPayload.empty() && cleanPayload[0] == '!')
                cleanPayload = cleanPayload.substr(1);
            SensorData data;
            if (tryParseSensorData(cleanPayload, data))
            {
                std::cout << u8"[异常上报] "
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
                logMessage(std::string(u8"[异常上报] ") + payload);
        }
    });

    while (g_running)
    {
        char manualCmd;
        while (SensorDisplay::popCommand(manualCmd))
        {
            if (manualCmd == '8' && g_rules)
            {
                g_rules->clearAlarm();
                logMessage(u8"[手动] 清除温度报警（指令 8）");
            }
            if (g_display) g_display->applyCommand(manualCmd);
            mqtt.publish("esp32/cmd", std::string(1, manualCmd));
            logMessage(std::string(u8"[手动] MQTT指令 ") + manualCmd +
                       u8" → " + cmdLabel(manualCmd));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    mqtt.end();
    g_running = false;
    g_errorCv.notify_all();
    if (errorThread.joinable())
        errorThread.join();
}

// 极简 TCP 指令发送：Winsock 直接发 CMD:x，无对象无线程
static bool sendCmdRaw(const std::string &host, int port, char cmd)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return false;

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    struct hostent *he = gethostbyname(host.c_str());
    if (!he) { closesocket(s); return false; }
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    // 3 秒连接超时
    int timeout = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    if (::connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    { closesocket(s); return false; }

    std::string msg = std::string("CMD:") + cmd + "\n";
    ::send(s, msg.c_str(), (int)msg.size(), 0);
    Sleep(200);  // 等待 TCP 数据发送完成
    closesocket(s);
    return true;
}

// 服务端模式（运行在数据线程中）
// 直接从服务端下载日志文件进行分析，不再依赖长连接推送
static void runServerMode()
{
    logMessage(u8"【服务端模式】8.137.48.225:9090");

    if (g_display) g_display->setConnected(true);

    DataLogger logger("temp/sensor_log.txt");
    g_logger = &logger;

    AnalysisRules rules;
    g_rules = &rules;

    // 设为 60 秒前，让首次循环立即触发下载
    auto lastAnalysisTime = std::chrono::system_clock::now() - std::chrono::seconds(60);

    while (g_running)
    {
        auto now = std::chrono::system_clock::now();

        // 每 60 秒下载日志并分析（服务端每 5 分钟才写一条新日志）
        if (now - lastAnalysisTime >= std::chrono::seconds(60))
        {
            lastAnalysisTime = now;

            std::vector<std::string> logLines;
            // 每次创建临时连接下载当日日志（当日 00:00:00 → 现在）
            std::time_t t_now = std::chrono::system_clock::to_time_t(now);
            std::tm tm_now;
            localtime_s(&tm_now, &t_now);
            tm_now.tm_hour = 0; tm_now.tm_min = 0; tm_now.tm_sec = 0;
            auto dayBegin = std::chrono::system_clock::from_time_t(std::mktime(&tm_now));
            ServerClient fetcher;
            if (fetcher.connect("8.137.48.225", 9090) &&
                fetcher.fetchLogs(toIsoString(dayBegin),
                                  toIsoString(now), logLines))
            {
                if (!logLines.empty())
                {
                    logMessage(std::string(u8"[分析] 下载到 ") +
                               std::to_string(logLines.size()) + u8" 条日志");

                    // === 第一步：更新图表（放在分析之前，避免 for 循环问题） ===
                    std::deque<double> tHist, sHist, lHist;
                    SensorData latestData;
                    for (const auto &line : logLines)
                    {
                        if (line.find("[ANOMALY]") != std::string::npos) continue;
                        SensorData d;
                        if (parseLogLine(line, d))
                        {
                            tHist.push_back(d.temperature);
                            sHist.push_back(d.soilMoisture);
                            lHist.push_back(d.lightIntensity);
                            latestData = d;
                        }
                    }
                    if (g_display && !tHist.empty())
                    {
                        g_display->setHistory(tHist, sHist, lHist);
                        g_display->updateData(latestData);
                        logMessage(std::string(u8"[图表] 历史 ") +
                                   std::to_string(tHist.size()) + u8" 条");
                    }

                    // === 第二步：逐条分析（可能耗时但图表已更新，不影响） ===
                    for (const auto &line : logLines)
                    {
                        if (line.find("[ANOMALY]") != std::string::npos) continue;
                        SensorData data;
                        if (parseLogLine(line, data))
                        {
                            if (g_display)
                            {
                                AnalysisConfig cfg = rules.getConfig();
                                cfg.growingArea = g_display->getGrowingArea();
                                rules.updateConfig(cfg);
                            }
                            rules.feedDataPoint(data);
                            auto cmds = rules.checkUrgent();
                            for (char cmd : cmds)
                            {
                                sendCmdRaw("8.137.48.225", 9090, cmd);
                                if (g_display) g_display->applyCommand(cmd);
                                logMessage(std::string(u8"[分析] 指令 ") + cmd + u8" → " + cmdLabel(cmd));
                            }
                            if (g_display)
                                g_display->setAnalysisData(
                                    rules.totalWaterUsed(), rules.tempAlarmActive(),
                                    rules.avgPpfd(), rules.currentDli(),
                                    rules.avgTemperature(), rules.isIrrigating(),
                                    rules.dataPointCount());
                        }
                    }
                }
            }
            fetcher.disconnect();
        }

        // 手动指令已移至 UI 主循环处理（数据线程不再处理，避免竞争）
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
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

    std::thread dataThread([&reader]() {
        runServerMode();
        if (!g_running) return;
        runMqttMode();
        if (!g_running) return;
        runRealMode(reader);
        if (!g_running) return;
        logMessage(u8"[错误] 所有数据源均不可用，程序退出");
        g_running = false;
    });

    while (display.isOpen() && g_running)
    {
        display.processEvents();
        display.render();

        // UI 线程直接处理手动指令（不依赖数据线程，响应更快）
        // UI 线程处理手动指令（唯一处理者，无竞争）
        char c;
        while (SensorDisplay::popCommand(c))
        {
            if (c == '8' && g_rules) g_rules->clearAlarm();
            if (g_display) g_display->applyCommand(c);
            bool ok = sendCmdRaw("8.137.48.225", 9090, c);
            logMessage(std::string(u8"[手动] ") + (ok ? u8"服务端指令 " : u8"发送失败 ") +
                       c + u8" → " + cmdLabel(c));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    g_running = false;
    if (dataThread.joinable())
        dataThread.join();

    reader.close();
    return 0;
}
