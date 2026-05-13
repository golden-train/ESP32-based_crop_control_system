#include "data_logger.hpp"
#include <iomanip>
#include <ctime>
#include <chrono>
#include <iostream>

DataLogger::DataLogger(const std::string &filePath)
{
    // 追加模式打开，每次运行不覆盖已有日志
    m_file.open(filePath, std::ios::app);

    if (!m_file.is_open())
        std::cerr << "[错误] 无法打开日志文件: " << filePath << std::endl;
    else
        std::cout << "[日志] 写入 " << filePath << std::endl;
}

DataLogger::~DataLogger()
{
    if (m_file.is_open())
        m_file.close();
}

void DataLogger::log(double temperature, double humidity,
                     double soilMoisture, double lightIntensity)
{
    // 获取当前系统时间
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;

    // 格式: [2026-05-13 10:30:25.123]  温度: 25.3℃  湿度: 55.1%  土壤: 42.7%  光照: 813lux
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_file.is_open())
        return;

    m_file << "[" << std::put_time(std::localtime(&t), "%Y-%m-%d %H:%M:%S")
           << "." << std::setfill('0') << std::setw(3) << ms.count() << "]  "
           << "温度: " << temperature << "℃  "
           << "湿度: " << humidity << "%  "
           << "土壤: " << soilMoisture << "%  "
           << "光照: " << lightIntensity << "lux"
           << std::endl;
}
