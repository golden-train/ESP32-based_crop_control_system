#pragma once

#include "sensor_data.hpp"
#include <windows.h>

// 分析配置（UI 可修改，线程安全）
typedef struct {
    double growingArea;    // 种植面积 (m²)
    double pumpFlowRate;   // 水泵流量 (L/min)
    double parCoeff;       // lux → PPFD 转换系数
    double parMin;         // PAR 不足阈值 (μmol/m²/s)
    double parMax;         // PAR 充足阈值 (μmol/m²/s)
} AnalysisConfig;

// 环形缓冲区（替代 std::deque）
#define RING_MAX 500

typedef struct {
    double data[RING_MAX];
    int head;
    int tail;
    int count;
} RingBuffer;

// 规则分析器：趋势检测、PAR 计算、阈值判断、用水量跟踪
typedef struct {
    RingBuffer tempHistory;
    RingBuffer soilHistory;
    RingBuffer lightHistory;

    // 趋势
    int risingCount;
    int trendAlarmActive;
    int trendAlarmSent;

    // PAR
    double ppfdSum;
    double lightSum;
    int    dataPointCount;

    // 灌溉
    int     irrigating;
    DWORD64 irrigateStartMs;
    double  totalWaterUsed;

    // 温度报警
    int tempAlarmActive;
    int tempAlarmSent;

    // 配置
    AnalysisConfig  config;
    CRITICAL_SECTION configMutex;
} AnalysisRules;

// 初始化
void AnalysisRules_init(AnalysisRules* rules);

// 每 5 分钟：存入一个数据点
void AnalysisRules_feedDataPoint(AnalysisRules* rules, const SensorData* data);

// 每 5 分钟：趋势检测 + PAR 控制 + 阈值判断，返回指令列表
// 返回写入 cmds 的指令个数
int AnalysisRules_checkUrgent(AnalysisRules* rules, char* cmds, int maxCmds);

// 收到 '8' 时清除温度报警
void AnalysisRules_clearAlarm(AnalysisRules* rules);

// 配置读写（线程安全）
void AnalysisRules_updateConfig(AnalysisRules* rules, const AnalysisConfig* cfg);
AnalysisConfig AnalysisRules_getConfig(const AnalysisRules* rules);

// === 状态查询 ===
double AnalysisRules_totalWaterUsed(const AnalysisRules* rules);
int    AnalysisRules_tempAlarmActive(const AnalysisRules* rules);
double AnalysisRules_avgPpfd(const AnalysisRules* rules);
double AnalysisRules_currentDli(const AnalysisRules* rules);
double AnalysisRules_avgTemperature(const AnalysisRules* rules);
int    AnalysisRules_isIrrigating(const AnalysisRules* rules);
int    AnalysisRules_dataPointCount(const AnalysisRules* rules);
int    AnalysisRules_isTrendAlarm(const AnalysisRules* rules);
