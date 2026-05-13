#pragma once

#include "sensor_data.hpp"

// 模拟传感器：当串口未连接时生成演示数据
// 内部使用随机种子产生有波动的温度、湿度、土壤湿度、光照值
class SimulatedSensor
{
public:
    // 生成一帧模拟数据
    static SensorData generate();
};
