#pragma once

#include <string>
#include <fstream>
#include <mutex>

// 数据记录器：给传感器数据加上时间戳，写入 txt 日志文件
class DataLogger
{
public:
    // filePath: 日志文件路径，如 "../temp/sensor_log.txt"
    explicit DataLogger(const std::string &filePath);
    ~DataLogger();

    // 追加一行记录：时间戳 + 温度 + 湿度 + 土壤湿度 + 光照强度
    void log(double temperature, double humidity,
             double soilMoisture, double lightIntensity);

    // 追加异常数据记录：与 log() 格式相同，但行中标记 [异常]
    void logAnomaly(double temperature, double humidity,
                    double soilMoisture, double lightIntensity);

private:
    std::ofstream m_file;
    std::mutex m_mutex;
};
