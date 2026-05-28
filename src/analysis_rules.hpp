#pragma once

#include "sensor_data.hpp"
#include <windows.h>
#include <time.h>

// 作物类型
enum CropType { Crop_Lettuce = 0, Crop_Strawberry = 1 };

// 分析配置（UI 可修改，线程安全）
typedef struct {
    double growingArea;    // 种植面积 (m²)
    double pumpFlowRate;   // 水泵流量 (L/min)
    double parCoeff;       // lux → PPFD 转换系数
    CropType cropType;     // 当前作物类型
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

    // DLI
    double lightSum;
    int    dataPointCount;
    double dliAccum;         // 全天累计 Σ(PPFD × Δt_sec)
    double hourAccum;        // 当前小时累计 Σ(PPFD × Δt_sec)
    double lastPpfd;         // 上一个数据点的 PPFD
    time_t lastTs;           // 上一个数据点的时间戳
    int    currentHour;      // 当前累计的小时 (0-23, -1=未初始化)

    // 灌溉
    int     irrigating;
    DWORD64 irrigateStartMs;
    time_t  irrigateStartTime;
    double  totalWaterUsed;

    // 日期（跨天重置所有累计）
    int lastDay;

    // 温度报警
    int tempAlarmActive;
    int tempAlarmSent;

    // 配置
    AnalysisConfig  config;
    CRITICAL_SECTION configMutex;
} AnalysisRules;

// 初始化
void AnalysisRules_init(AnalysisRules* rules);

// 存入一个数据点，ts 为数据实际时间戳（0=使用当前时间）
void AnalysisRules_feedDataPoint(AnalysisRules* rules, const SensorData* data, time_t ts);

// 趋势检测 + 控制判断，返回指令列表
int AnalysisRules_checkUrgent(AnalysisRules* rules, char* cmds, int maxCmds);

// 收到 '8' 时清除温度报警
void AnalysisRules_clearAlarm(AnalysisRules* rules);

// 切换作物类型（UI 设置时调用）
void AnalysisRules_selectCrop(AnalysisRules* rules, CropType crop);

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
