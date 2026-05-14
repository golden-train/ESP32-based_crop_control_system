#pragma once

#include "sensor_data.hpp"
#include <vector>
#include <string>
#include <deque>
#include <chrono>
#include <mutex>

// 分析配置（UI 可修改，线程安全）
struct AnalysisConfig
{
    double growingArea   = 1.0;   // 种植面积 (m²)
    double pumpFlowRate  = 2.0;   // 水泵流量 (L/min)  ← 占位，请按实际修改
    double parCoeff      = 0.0185;// lux → PPFD 转换系数   ← 占位，需标定
    double parMin        = 5.0;  // PAR 不足阈值 (μmol/m²/s)  ← 室内测试调小
    double parMax        = 15.0; // PAR 充足阈值 (μmol/m²/s)  ← 室内测试调小
};

// 规则分析器：负责趋势检测、PAR 计算、阈值判断、用水量跟踪
// 全部历史数据点来自每 5 分钟的分析采样
class AnalysisRules
{
public:
    AnalysisRules();

    // 每 5 分钟：存入一个数据点（供趋势 + PAR 累计）
    void feedDataPoint(const SensorData &data);

    // 每 5 分钟：趋势检测 + PAR 控制 + 阈值判断，返回指令列表
    std::vector<char> checkUrgent();

    // 收到 '8' 时清除温度报警（仅关蜂鸣器，不清除其他状态）
    void clearAlarm();

    // 配置读写（线程安全）
    void updateConfig(const AnalysisConfig &cfg);
    AnalysisConfig getConfig() const;

    // === 状态查询（供 UI 显示） ===
    double totalWaterUsed()   const { return m_totalWaterUsed; }
    bool   tempAlarmActive()  const { return m_tempAlarmActive; }
    double avgPpfd()          const; // 全程平均 PPFD (μmol/m²/s)
    double currentDli()       const; // 当前累计 DLI (mol/m²)
    double avgTemperature()   const; // 全程平均温度
    bool   isIrrigating()     const { return m_irrigating; }
    int    dataPointCount()   const { return m_dataPointCount; }
    bool   isTrendAlarm()     const { return m_trendAlarmActive; }

private:
    // 趋势检测：用全部历史温度数据点判断是否持续上升
    bool detectRisingTrend();

    // 计算本次灌溉用水量并累加
    void accumulateWaterUsage();

    // === 全部5分钟数据点历史 ===
    std::deque<double> m_tempHistory;
    std::deque<double> m_soilHistory;
    std::deque<double> m_lightHistory;
    static constexpr size_t MAX_HISTORY = 500; // 约 41 小时（5分钟粒度）

    // === 趋势 ===
    int     m_risingCount = 0;
    bool    m_trendAlarmActive = false;
    bool    m_trendAlarmSent = false;

    // === PAR ===
    double  m_ppfdSum = 0;      // PPFD 累计值
    double  m_lightSum = 0;     // 光照累计值（用于平均）
    int     m_dataPointCount = 0;

    // === 灌溉 ===
    bool    m_irrigating = false;
    std::chrono::steady_clock::time_point m_irrigateStart;
    double  m_totalWaterUsed = 0;

    // === 温度报警 ===
    bool    m_tempAlarmActive = false;
    bool    m_tempAlarmSent = false;

    // === 配置 ===
    AnalysisConfig m_config;
    mutable std::mutex m_configMutex;
};
