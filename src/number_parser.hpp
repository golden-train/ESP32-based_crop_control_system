#pragma once

#include "sensor_data.hpp"
#include <time.h>

// 从一行文本中提取所有数字（支持整数和小数）
// 返回成功提取的个数，最多 maxCount 个，写入 out 数组
int extractNumbers(const char* line, double* out, int maxCount);

// 从一行文本解析 SensorData（需要至少 4 个数值）
// 返回 1=成功，0=失败
int tryParseSensorData(const char* line, SensorData* out);

// 从服务端日志行解析传感器数据和时间戳
// 格式: "[2026-05-14 10:00:00] 25.3 55.1 42.0 813"
// 格式: "[2026-05-14 10:00:03][ANOMALY] 25.3 55.1 42.0 813"
// ts 输出时间戳（可为 NULL），0 表示解析失败
int parseLogLine(const char* line, SensorData* out, time_t* ts);
