#include "simulated_sensor.hpp"
#include <random>
#include <chrono>

SensorData SimulatedSensor::generate()
{
    // 基于当前时间播种，每次调用产生微小波动
    static std::mt19937 rng(
        static_cast<unsigned>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    static std::uniform_real_distribution<double> tempJitter(-0.5, 0.5);    // 温度波动 ±0.5°C
    static std::uniform_real_distribution<double> humJitter(-1.5, 1.5);     // 湿度波动 ±1.5%
    static std::uniform_real_distribution<double> soilJitter(-1.0, 1.0);    // 土壤湿度波动 ±1%
    static std::uniform_real_distribution<double> lightJitter(-30.0, 30.0); // 光照波动 ±30lux

    // 基础值模拟一个典型室内环境
    SensorData data;
    data.temperature   = 25.0 + tempJitter(rng);   // ~25°C
    data.humidity      = 55.0 + humJitter(rng);     // ~55%
    data.soilMoisture  = 42.0 + soilJitter(rng);    // ~42%
    data.lightIntensity = 800.0 + lightJitter(rng); // ~800 lux

    return data;
}
