/*
 * data_source：数据源模式实现。
 *
 * 每个 runXxxMode 函数独立创建 AnalysisRules 栈实例，
 * 设置全局指针供子线程/回调访问。退出时自动清理。
 *
 * 包含：winsock2.h 先于 windows.h
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

#define SAMPLE_INTERVAL_SEC 1

// ============================================================ //
//                  服务端模式（runServerMode）                   //
// ============================================================ //

static void runServerMode(void)
{
    logMessage("[服务端模式] 8.137.48.225:9090");
    if (g_display) g_display->setConnected(true);

    AnalysisRules rules;
    AnalysisRules_init(&rules);
    g_rules = &rules;

    time_t lastAnalysisTime = time(NULL) - 60;

    while (g_running)
    {
        time_t now = time(NULL);

        if (now - lastAnalysisTime >= 60)
        {
            lastAnalysisTime = now;

            struct tm tmNow;
            localtime_s(&tmNow, &now);
            struct tm dayBegin = tmNow;
            dayBegin.tm_hour = 0; dayBegin.tm_min = 0; dayBegin.tm_sec = 0;
            time_t tBegin = mktime(&dayBegin);

            char beginStr[32], endStr[32];
            toIsoString(tBegin, beginStr, sizeof(beginStr));
            toIsoString(now, endStr, sizeof(endStr));

            ServerClient fetcher;
            ServerClient_init(&fetcher);
            char logLines[512][256];
            int logCount = 0;

            if (ServerClient_connect(&fetcher, "8.137.48.225", 9090) &&
                ServerClient_fetchLogs(&fetcher, beginStr, endStr,
                                       logLines, &logCount, 512))
            {
                if (logCount > 0)
                {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "[分析] 下载到 %d 条日志", logCount);
                    logMessage(buf);

                    /* 第一步：更新图表 */
                    double tHist[512], sHist[512], lHist[512];
                    int histCount = 0;
                    SensorData latestData;

                    for (int i = 0; i < logCount; i++)
                    {
                        if (strstr(logLines[i], "[ANOMALY]")) continue;
                        SensorData d;
                        if (parseLogLine(logLines[i], &d))
                        {
                            tHist[histCount] = d.temperature;
                            sHist[histCount] = d.soilMoisture;
                            lHist[histCount] = d.lightIntensity;
                            latestData = d;
                            histCount++;
                        }
                    }

                    if (g_display && histCount > 0)
                    {
                        g_display->setHistory(tHist, histCount,
                                               sHist, histCount,
                                               lHist, histCount);
                        g_display->updateData(latestData);
                        snprintf(buf, sizeof(buf), "[图表] 历史 %d 条", histCount);
                        logMessage(buf);
                    }

                    /* 第二步：逐条分析 */
                    for (int i = 0; i < logCount; i++)
                    {
                        if (strstr(logLines[i], "[ANOMALY]")) continue;
                        SensorData data;
                        if (parseLogLine(logLines[i], &data))
                        {
                            if (g_display)
                            {
                                AnalysisConfig cfg = AnalysisRules_getConfig(&rules);
                                cfg.growingArea = g_display->getGrowingArea();
                                AnalysisRules_updateConfig(&rules, &cfg);
                            }
                            AnalysisRules_feedDataPoint(&rules, &data);
                            char cmds[8];
                            int n = AnalysisRules_checkUrgent(&rules, cmds, 8);
                            for (int j = 0; j < n; j++)
                            {
                                sendCmdRaw("8.137.48.225", 9090, cmds[j]);
                                if (g_display) g_display->applyCommand(cmds[j]);
                                char msg[128];
                                snprintf(msg, sizeof(msg), "[分析] 指令 %c → %s",
                                         cmds[j], cmdLabel(cmds[j]));
                                logMessage(msg);
                            }
                            if (g_display)
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
                }
            }
            ServerClient_disconnect(&fetcher);
        }

        Sleep(100);
    }
}

// ============================================================ //
//                   数据线程入口                                 //
// ============================================================ //

DWORD WINAPI dataThreadFunc(LPVOID arg)
{
    (void)arg;

    runServerMode();

    logMessage("[错误] 服务端连接失败，程序退出");
    g_running = 0;
    return 0;
}
