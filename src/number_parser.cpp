#include "number_parser.hpp"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

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

/* 从服务端日志行（带时间戳 [xxxx]）解析 SensorData，跳过方括号部分 */
int parseLogLine(const char* line, SensorData* out)
{
    const char* bracketEnd = strrchr(line, ']');
    if (!bracketEnd)
        return tryParseSensorData(line, out);
    return tryParseSensorData(bracketEnd + 1, out);
}
