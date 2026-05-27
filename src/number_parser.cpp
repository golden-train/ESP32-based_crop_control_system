#include "number_parser.hpp"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * 从一行文本中提取所有浮点数。
 * 逐字符扫描，遇到数字/小数点/负号累积到临时缓冲区，遇到分隔符则 strtod 转换。
 */
int extractNumbers(const char* line, double* out, int maxCount)
{
    int count = 0;
    const char* p = line;
    char buf[64];
    int bufPos = 0;

    while (*p && count < maxCount)
    {
        char ch = *p;
        if (isdigit((unsigned char)ch) || ch == '.' || ch == '-')
        {
            if (bufPos < (int)sizeof(buf) - 1)
                buf[bufPos++] = ch;
        }
        else if (bufPos > 0)
        {
            buf[bufPos] = '\0';
            out[count++] = strtod(buf, NULL);
            bufPos = 0;
        }
        p++;
    }

    if (bufPos > 0 && count < maxCount)
    {
        buf[bufPos] = '\0';
        out[count++] = strtod(buf, NULL);
    }

    return count;
}

/* 从一行文本解析 SensorData：至少需要 4 个数值，顺序为 温度/湿度/土壤/光照 */
int tryParseSensorData(const char* line, SensorData* out)
{
    double vals[4];
    int n = extractNumbers(line, vals, 4);
    if (n >= 4)
    {
        out->temperature    = vals[0];
        out->humidity       = vals[1];
        out->soilMoisture   = vals[2];
        out->lightIntensity = vals[3];
        return 1;
    }
    return 0;
}

/* 解析日志行开头的时间戳 "[yyyy-MM-dd HH:mm:ss]" 或 "[HH:mm:ss]" */
static time_t parseTimestamp(const char* line)
{
    if (line[0] != '[') return 0;
    line++;

    struct tm tm = {0};
    int n;

    /* 完整格式: "2026-05-14 10:00:00]" */
    n = sscanf(line, "%d-%d-%d %d:%d:%d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
    if (n == 6)
    {
        tm.tm_year -= 1900;
        tm.tm_mon  -= 1;
        tm.tm_isdst = -1;
        return mktime(&tm);
    }

    /* 短格式: "10:00:00]" */
    n = sscanf(line, "%d:%d:%d", &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
    if (n == 3)
    {
        time_t now = time(NULL);
        struct tm tmNow;
        localtime_s(&tmNow, &now);
        tm.tm_year  = tmNow.tm_year;
        tm.tm_mon   = tmNow.tm_mon;
        tm.tm_mday  = tmNow.tm_mday;
        tm.tm_isdst = -1;
        return mktime(&tm);
    }

    return 0;
}

/* 从服务端日志行解析 SensorData + 时间戳 */
int parseLogLine(const char* line, SensorData* out, time_t* ts)
{
    if (ts) *ts = parseTimestamp(line);

    const char* bracketEnd = strrchr(line, ']');
    if (!bracketEnd)
        return tryParseSensorData(line, out);
    return tryParseSensorData(bracketEnd + 1, out);
}
