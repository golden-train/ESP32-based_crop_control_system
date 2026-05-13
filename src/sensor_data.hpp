#pragma once

// 传感器数据结构：存储从ESP32接收的一次完整读数
struct SensorData
{
    double temperature = 0.0;    // 环境温度 (°C)
    double humidity = 0.0;       // 环境湿度 (%)
    double soilMoisture = 0.0;   // 土壤湿度 (%)
    double lightIntensity = 0.0; // 光照强度 (lux)
};
