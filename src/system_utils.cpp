/*
 * system_utils：系统公共工具模块实现。
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>

#include "system_utils.hpp"
#include "number_parser.hpp"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── 全局变量定义 ──────────────────────────────────────── */
SensorDisplay *g_display    = NULL;
AnalysisRules *g_rules      = NULL;

volatile int g_currentMode       = 0;  /* 0=服务端, 1=演示 */
volatile int g_modeTogglePending = 0;
volatile int g_weatherCode       = -1; /* WMO 天气码，-1=未获取 */

/* ── 工具函数 ────────────────────────────────────────────── */

void logMessage(const char *msg)
{
    printf("%s\n", msg);
    if (g_display)
        g_display->addLog(msg);
}

void processData(const SensorData *data)
{
    printf("[传感器] 温度: %.1f℃  湿度: %.1f%%  土壤: %.1f%%  光照: %.1flux\n",
           data->temperature, data->humidity,
           data->soilMoisture, data->lightIntensity);
    if (g_display)
        g_display->updateData(*data);
}

const char *cmdLabel(char cmd)
{
    switch (cmd)
    {
    case '1': return "开补光";
    case '3': return "蜂鸣器";
    case '4': return "开灌溉";
    case '5': return "关补光";
    case '6': return "关灌溉";
    case '8': return "清除报警";
    case '9': return "关闭所有";
    case 'T': return "立即上报";
    default:  return "未知";
    }
}

void toIsoString(time_t t, char *buf, int bufSize)
{
    struct tm tm;
    localtime_s(&tm, &t);
    strftime(buf, bufSize, "%Y-%m-%dT%H:%M:%S", &tm);
}

int sendCmdRaw(const char *host, int port, char cmd)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return 0;

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    struct hostent *he = gethostbyname(host);
    if (!he) { closesocket(s); return 0; }
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    int timeout = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    { closesocket(s); return 0; }

    char msg[32];
    snprintf(msg, sizeof(msg), "CMD:%c\n", cmd);
    send(s, msg, (int)strlen(msg), 0);
    Sleep(200);
    closesocket(s);
    return 1;
}

/* 向服务端查询最新一条传感器数据，成功返回 1，line 格式如 "25.3 55.1 42 813" */
int fetchLatestRaw(const char *host, int port, char *line, int maxLen)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return 0;

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    struct hostent *he = gethostbyname(host);
    if (!he) { closesocket(s); return 0; }
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    int timeout = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    { closesocket(s); return 0; }

    send(s, "GET_LATEST\n", 11, 0);
    Sleep(200);

    char buf[256];
    int n = recv(s, buf, sizeof(buf) - 1, 0);
    closesocket(s);

    if (n <= 0) return 0;
    buf[n] = '\0';

    /* 服务端回复 "LATEST:25.3 55.1 42 813\n" 或 "LATEST:NODATA\n" */
    const char *prefix = "LATEST:";
    char *p = strstr(buf, prefix);
    if (!p) return 0;
    p += strlen(prefix);

    /* 去除行尾换行符 */
    size_t len = strlen(p);
    while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r'))
        p[--len] = '\0';

    if (strcmp(p, "NODATA") == 0) return 0;

    strncpy_s(line, maxLen, p, _TRUNCATE);
    return 1;
}

void processManualCommands(void)
{
    char c;
    while (SensorDisplay::popCommand(c))
    {
        if (c == '8' && g_rules)
        {
            AnalysisRules_clearAlarm(g_rules);
            logMessage("[手动] 清除温度报警（指令 8）");
        }

        if (g_display)
            g_display->applyCommand(c);

        char buf[128];
        snprintf(buf, sizeof(buf), "[手动] 指令 %c → %s", c, cmdLabel(c));
        logMessage(buf);
    }
}

void runAnalysisPoint(SensorData *data)
{
    if (!g_rules) return;

    /* 0. 同步 UI 配置 */
    if (g_display)
    {
        AnalysisConfig cfg = AnalysisRules_getConfig(g_rules);
        cfg.growingArea = g_display->getGrowingArea();
        cfg.cropType = (CropType)g_display->getCropType();
        AnalysisRules_updateConfig(g_rules, &cfg);
    }

    /* 1. 存入数据点 */
    AnalysisRules_feedDataPoint(g_rules, data, time(NULL));

    /* 2. 执行分析 */
    char cmds[8];
    int nCmds = AnalysisRules_checkUrgent(g_rules, cmds, 8);

    /* 3. 发送指令 */
    for (int i = 0; i < nCmds; i++)
    {
        char c = cmds[i];
        if (g_display)
            g_display->applyCommand(c);

        char msg[128];
        snprintf(msg, sizeof(msg), "[分析] 指令 %c → %s", c, cmdLabel(c));
        logMessage(msg);
    }

    /* 4. 推送分析数据到 UI */
    if (g_display)
    {
        g_display->setAnalysisData(
            AnalysisRules_totalWaterUsed(g_rules),
            AnalysisRules_tempAlarmActive(g_rules) ? true : false,
            AnalysisRules_avgPpfd(g_rules),
            AnalysisRules_currentDli(g_rules),
            AnalysisRules_avgTemperature(g_rules),
            AnalysisRules_isIrrigating(g_rules) ? true : false,
            AnalysisRules_dataPointCount(g_rules));
    }

    /* 5. 输出分析摘要 */
    char buf[256];
    snprintf(buf, sizeof(buf),
             "[分析] 数据点 #%d | 温度趋势: %s | 平均温度: %.1f℃"
             " | 用水量: %.1fL%s",
             AnalysisRules_dataPointCount(g_rules),
             AnalysisRules_isTrendAlarm(g_rules) ? "↑报警" : "正常",
             AnalysisRules_avgTemperature(g_rules),
             AnalysisRules_totalWaterUsed(g_rules),
             AnalysisRules_tempAlarmActive(g_rules) ? " | [!] 温度报警" : "");
    logMessage(buf);
}

/* UI 线程处理手动指令队列 */
void processUiCommands(void)
{
    char c;
    while (SensorDisplay::popCommand(c))
    {
        if (c == '8' && g_rules) { AnalysisRules_clearAlarm(g_rules); if (g_display && g_rules) { g_display->setAnalysisData(AnalysisRules_totalWaterUsed(g_rules), false, AnalysisRules_avgPpfd(g_rules), AnalysisRules_currentDli(g_rules), AnalysisRules_avgTemperature(g_rules), AnalysisRules_isIrrigating(g_rules), AnalysisRules_dataPointCount(g_rules)); } }
        if (g_display) g_display->applyCommand(c);

        char buf[128];
        int ok = sendCmdRaw(SERVER_HOST, SERVER_PORT, c);
        snprintf(buf, sizeof(buf), "[手动] %s %c → %s",
                 ok ? "服务端指令" : "发送失败", c, cmdLabel(c));
        logMessage(buf);

        /* 'T'：触发立即上报，刷新当前仪表值但不改折线图 */
        if (c == 'T' && ok)
        {
            Sleep(500);
            char latest[128];
            if (fetchLatestRaw(SERVER_HOST, SERVER_PORT, latest, sizeof(latest)))
            {
                SensorData data;
                if (tryParseSensorData(latest, &data))
                {
                    if (g_display) g_display->setCurrentData(data);
                    logMessage("[触发] 已获取最新数据（仪表刷新，折线图不变）");
                }
            }
        }
    }
}

const char* weatherLabel(int code)
{
    if (code < 0) return "未知";
    switch (code) {
    case 0:  return "晴";
    case 1:  return "少云";
    case 2:  return "多云";
    case 3:  return "阴";
    case 45: case 48: return "雾";
    case 51: case 53: case 55: return "毛毛雨";
    case 61: case 63: case 65: return "雨";
    case 71: case 73: case 75: return "雪";
    case 80: case 81: case 82: return "阵雨";
    case 95: case 96: case 99: return "雷暴";
    default: return "其他";
    }
}

int fetchWeather(double lat, double lon)
{
    HINTERNET hSession = WinHttpOpen(L"SensorMonitor/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return -1;

    HINTERNET hConnect = WinHttpConnect(hSession,
        L"api.open-meteo.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return -1; }

    wchar_t path[256];
    swprintf(path, 256,
        L"/v1/forecast?latitude=%.2f&longitude=%.2f"
        L"&daily=weather_code&timezone=auto&forecast_days=1",
        lat, lon);

    HINTERNET hRequest = WinHttpOpenRequest(hConnect,
        L"GET", path, NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }

    int code = -1;
    if (WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, NULL))
    {
        char buf[4096];
        DWORD read = 0;
        int total = 0;
        while (WinHttpReadData(hRequest, buf + total,
                sizeof(buf) - 1 - total, &read) && read > 0)
        {
            total += read;
            if (total >= (int)sizeof(buf) - 1) break;
        }
        buf[total] = '\0';

        const char* p = strstr(buf, "\"weather_code\":[");
        if (p) { p += 16; code = atoi(p); }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return code;
}
