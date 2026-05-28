/*
 * data_source：数据源模式实现。
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include "data_source.hpp"
#include "server_client.hpp"
#include "number_parser.hpp"
#include "system_utils.hpp"
#include "error_handler.hpp"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SAMPLE_INTERVAL_SEC 1
#define USE_DEMO 1

static int refreshWeather(void);

// ============================================================ //
//                  服务端模式                                    //
// ============================================================ //

static void runServerMode(void)
{
    logMessage("[服务端模式] 8.137.48.225:9090");
    if (g_display) g_display->setConnected(true);

    AnalysisRules rules;
    AnalysisRules_init(&rules);
    g_rules = &rules;

    time_t lastFetchTime = time(NULL) - 60;
    int lastWeatherDay = -1;

    while (g_running && !g_modeTogglePending)
    {
        time_t now = time(NULL);

        /* ── 每日天气刷新 ── */
        struct tm tmNow;
        localtime_s(&tmNow, &now);
        if (tmNow.tm_yday != lastWeatherDay) {
            refreshWeather();
            lastWeatherDay = tmNow.tm_yday;
        }

        /* ── 每分钟拉一次日志 ── */
        if (now - lastFetchTime < 60) { Sleep(100); continue; }
        lastFetchTime = now;

        struct tm dayBegin = tmNow;
        dayBegin.tm_hour = 0; dayBegin.tm_min = 0; dayBegin.tm_sec = 0;
        time_t tBegin = mktime(&dayBegin);

        char beginStr[32], endStr[32];
        toIsoString(tBegin, beginStr, sizeof(beginStr));
        toIsoString(now, endStr, sizeof(endStr));

        /* 同步 UI 配置 */
        if (g_display)
        {
            AnalysisConfig cfg = AnalysisRules_getConfig(&rules);
            cfg.growingArea = g_display->getGrowingArea();
            cfg.cropType = (CropType)g_display->getCropType();
            AnalysisRules_updateConfig(&rules, &cfg);
        }

        ServerClient fetcher;
        ServerClient_init(&fetcher);
        char logLines[512][256];
        int logCount = 0;

        if (!ServerClient_connect(&fetcher, "8.137.48.225", 9090) ||
            !ServerClient_fetchLogs(&fetcher, beginStr, endStr,
                                     logLines, &logCount, 512))
        {
            ServerClient_disconnect(&fetcher);
            continue;
        }
        ServerClient_disconnect(&fetcher);

        if (logCount <= 0) continue;

        char buf[128];
        snprintf(buf, sizeof(buf), "[分析] 下载到 %d 条日志", logCount);
        logMessage(buf);

        // ================================================ //
        // 阶段一：积累数据（用真实时间戳喂数据点）          //
        // ================================================ //
        double tHist[512], sHist[512], lHist[512];
        int histCount = 0;
        SensorData latestData = {0};

        int anomalyDetected = 0;
        for (int i = 0; i < logCount; i++)
        {
            if (strstr(logLines[i], "[ANOMALY]"))
            {
                if (!anomalyDetected)
                {
                    anomalyDetected = 1;
                    rules.tempAlarmActive = 1;
                    logMessage("[告警] ESP32 检测到传感器数据异常！");
                }
                continue;
            }

            SensorData data;
            time_t ts = 0;
            if (!parseLogLine(logLines[i], &data, &ts)) continue;

            tHist[histCount] = data.temperature;
            sHist[histCount] = data.soilMoisture;
            lHist[histCount] = data.lightIntensity;
            latestData = data;
            histCount++;

            AnalysisRules_feedDataPoint(&rules, &data, ts);
        }

        // ================================================ //
        // 阶段二：单次决策                                   //
        // ================================================ //
        if (histCount > 0)
        {
            char cmds[8];
            int n = AnalysisRules_checkUrgent(&rules, cmds, 8);
            for (int j = 0; j < n; j++)
            {
                sendCmdRaw("8.137.48.225", 9090, cmds[j]);
                if (g_display) g_display->applyCommand(cmds[j]);
                snprintf(buf, sizeof(buf), "[分析] 指令 %c → %s",
                         cmds[j], cmdLabel(cmds[j]));
                logMessage(buf);
            }

            // ============================================ //
            // 阶段三：单次 UI 推送                           //
            // ============================================ //
            if (g_display)
            {
                g_display->setHistory(tHist, histCount,
                                      sHist, histCount,
                                      lHist, histCount);
                g_display->updateData(latestData);
                g_display->setAnalysisData(
                    AnalysisRules_totalWaterUsed(&rules),
                    AnalysisRules_tempAlarmActive(&rules) ? true : false,
                    AnalysisRules_avgPpfd(&rules),
                    AnalysisRules_currentDli(&rules),
                    AnalysisRules_avgTemperature(&rules),
                    AnalysisRules_isIrrigating(&rules) ? true : false,
                    AnalysisRules_dataPointCount(&rules));
            }

            snprintf(buf, sizeof(buf),
                     "[分析] 数据点 #%d | 用水量: %.1fL%s",
                     AnalysisRules_dataPointCount(&rules),
                     AnalysisRules_totalWaterUsed(&rules),
                     AnalysisRules_tempAlarmActive(&rules) ? " | 温度报警" : "");
            logMessage(buf);
        }
    }
}

// ============================================================ //
//                   演示模式                                    //
// ============================================================ //

static void runDemoMode(void)
{
    logMessage("[演示模式] 启动（1 tick=5分钟, 一天≈29秒）");
    if (g_display) g_display->setConnected(false);

    AnalysisRules rules;
    AnalysisRules_init(&rules);
    g_rules = &rules;

    int simMin = 360;
    double soil = 38;
    int histTick = 0;
    int irrTicks = 0;
    double simWater = 0;
    int lastWeatherDay = -1;

    while (g_running && !g_modeTogglePending)
    {
        int todMin = simMin % 1440;
        double angle = (todMin - 14 * 60) * 2 * M_PI / 1440;

        double temp = 25 + 5 * sin(angle);
        double hum  = 62 - 12 * sin(angle);
        double lux  = 0;
        if (todMin >= 360 && todMin < 1080)
        {
            double lAng = (todMin - 360) * M_PI / 720;
            lux = 33400 * sin(lAng);
        }

        if (rules.irrigating)
            soil += 0.25;
        else
            soil -= 0.2;
        if (soil > 75) soil = 75;
        if (soil < 5)  soil = 5;

        SensorData data = { temp, hum, soil, lux };

        if (g_display)
        {
            g_display->setCurrentData(data);
            if (++histTick >= 5) { histTick = 0;
                g_display->updateData(data); }
        }

        /* 演示用模拟时间戳喂 feedDataPoint */
        time_t simTs = time(NULL) + (simMin - 360) * 300;
        AnalysisRules_feedDataPoint(&rules, &data, simTs);

        /* 每 5 模拟分钟 = 每 tick 决策一次 */
        {
            char cmds[8];
            int n = AnalysisRules_checkUrgent(&rules, cmds, 8);
            for (int j = 0; j < n; j++) {
                if (g_display) g_display->applyCommand(cmds[j]);
            }
            if (g_display) {
                g_display->setAnalysisData(
                    AnalysisRules_totalWaterUsed(&rules),
                    AnalysisRules_tempAlarmActive(&rules) ? true : false,
                    AnalysisRules_avgPpfd(&rules),
                    AnalysisRules_currentDli(&rules),
                    AnalysisRules_avgTemperature(&rules),
                    AnalysisRules_isIrrigating(&rules) ? true : false,
                    AnalysisRules_dataPointCount(&rules));
            }
        }

        /* 演示用水量 */
        {
            if (rules.irrigating)
                irrTicks++;
            else if (irrTicks > 0)
            {
                simWater += 0.5 * (irrTicks * 5.0 / 60.0);
                irrTicks = 0;
                rules.totalWaterUsed = simWater;
            }
        }

        /* 每日天气刷新 */
        {
            time_t tw = time(NULL);
            struct tm tmw;
            localtime_s(&tmw, &tw);
            if (tmw.tm_yday != lastWeatherDay) {
                refreshWeather();
                lastWeatherDay = tmw.tm_yday;
            }
        }

        simMin += 5;
        Sleep(100);
    }
}

// ============================================================ //
//                   数据线程入口                                 //
// ============================================================ //

static int refreshWeather(void)
{
    int c = fetchWeather(29.83, 106.43);
    if (c >= 0) {
        g_weatherCode = c;
        char buf[64];
        snprintf(buf, sizeof(buf), "[天气] %s (码 %d)", weatherLabel(c), c);
        logMessage(buf);
        return 1;
    }
    return 0;
}

DWORD WINAPI dataThreadFunc(LPVOID arg)
{
    (void)arg;
    refreshWeather();

    while (g_running)
    {
        if (g_modeTogglePending)
        {
            g_currentMode = !g_currentMode;
            g_modeTogglePending = 0;
            if (g_display) g_display->clearAll();
            char buf[64];
            snprintf(buf, sizeof(buf), "[模式] 切换到 %s",
                     g_currentMode ? "演示模式" : "服务端模式");
            logMessage(buf);
        }

        if (g_currentMode) runDemoMode();
        else               runServerMode();
    }

    logMessage("[错误] 数据线程退出");
    g_running = 0;
    return 0;
}
