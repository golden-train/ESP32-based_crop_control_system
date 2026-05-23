#include "analysis_rules.hpp"
#include <math.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * ── 作物参数表 ────────────────────────────────────────────
 * 索引：Crop_Lettuce=0, Crop_Strawberry=1
 */
static const double CROP_DLI[]     = { 17, 22 };   // DLI 目标
static const double CROP_TMP_MAX[] = { 28, 32 };   // 温度报警阈值
static const double CROP_TMP_REL[] = { 26, 30 };   // 温度释放阈值
static const double CROP_TMP_IRR[] = { 26, 30 };   // 高温禁灌阈值
static const double CROP_SOI_DRY[] = { 22, 25 };   // 开灌溉
static const double CROP_SOI_WET[] = { 50, 55 };   // 关灌溉

static const double TREND_DELTA  = 0.5;      /* 趋势判断最小差值 (°C) */
static const int    TREND_COUNT  = 4;        /* 连续趋势报警次数 */

/*
 * ============================================================
 *                     环形缓冲区操作
 * ============================================================
 *
 * 使用 RING_MAX (500) 大小的定长数组实现 FIFO 队列。
 * head 指向最旧元素，tail 指向下一个写入位置。
 * 满时覆盖最旧元素（适合滑动窗口统计），无需动态内存分配。
 */

/* 尾端推入：满则覆盖头部最旧数据 */
static void ringPush(RingBuffer* rb, double val)
{
    if (rb->count >= RING_MAX)
    {
        rb->head = (rb->head + 1) % RING_MAX;
        rb->count--;
    }
    rb->data[rb->tail] = val;
    rb->tail = (rb->tail + 1) % RING_MAX;
    rb->count++;
}

static int ringSize(const RingBuffer* rb) { return rb->count; }

/* 相对索引访问：idx=0 返回头部（最旧），idx=count-1 返回尾部（最新） */
static double ringAt(const RingBuffer* rb, int idx)
{
    return rb->data[(rb->head + idx) % RING_MAX];
}

/* 全部元素求平均 */
static double ringAvg(const RingBuffer* rb)
{
    if (rb->count == 0) return 0;
    double sum = 0;
    for (int i = 0; i < rb->count; i++)
        sum += ringAt(rb, i);
    return sum / rb->count;
}

/*
 * ============================================================
 *                     AnalysisRules 实现
 * ============================================================
 */

/* 初始化所有字段为默认值 */
void AnalysisRules_init(AnalysisRules* rules)
{
    rules->tempHistory.head = rules->tempHistory.tail = rules->tempHistory.count = 0;
    rules->soilHistory.head = rules->soilHistory.tail = rules->soilHistory.count = 0;
    rules->lightHistory.head = rules->lightHistory.tail = rules->lightHistory.count = 0;

    rules->risingCount       = 0;
    rules->trendAlarmActive  = 0;
    rules->trendAlarmSent    = 0;

    rules->ppfdSum           = 0;
    rules->lightSum          = 0;
    rules->dataPointCount    = 0;

    rules->irrigating        = 0;
    rules->irrigateStartMs   = 0;
    rules->totalWaterUsed    = 0;

    rules->lastDay           = -1;

    rules->tempAlarmActive   = 0;
    rules->tempAlarmSent     = 0;

    /* 分析配置默认值 */
    rules->config.growingArea  = 0.1;     /* 种植面积 0.1 m² */
    rules->config.pumpFlowRate = 0.5;     /* 水泵流量 0.5 L/min */
    rules->config.parCoeff     = 0.0185;  /* lux→PPFD 转换系数（需标定） */
    rules->config.cropType     = Crop_Lettuce;

    InitializeCriticalSection(&rules->configMutex);
}

/*
 * 存入一个数据点：三个传感器值分别写入对应的环形缓冲区，
 * 同时累计 PPFD 和光照值用于全程平均计算。
 * 跨天时重置 DLI 累计，让新一天从零开始。
 */
void AnalysisRules_feedDataPoint(AnalysisRules* rules, const SensorData* data)
{
    /* 跨天检测 */
    time_t now = time(NULL);
    struct tm tmNow;
    localtime_s(&tmNow, &now);
    int today = tmNow.tm_yday;
    if (today != rules->lastDay)
    {
        rules->ppfdSum        = 0;
        rules->lightSum       = 0;
        rules->dataPointCount = 0;
        rules->lastDay        = today;
    }

    ringPush(&rules->tempHistory, data->temperature);
    ringPush(&rules->soilHistory, data->soilMoisture);
    ringPush(&rules->lightHistory, data->lightIntensity);

    /* PPFD 累计需要读取 config.parCoeff，加锁保护 */
    EnterCriticalSection(&rules->configMutex);
    double ppfd = data->lightIntensity * rules->config.parCoeff;
    rules->ppfdSum += ppfd;
    rules->lightSum += data->lightIntensity;
    rules->dataPointCount++;
    LeaveCriticalSection(&rules->configMutex);
}

/*
 * ── 趋势检测 ──────────────────────────────────────────────
 * 将温度历史数据分为前后两半，比较均值差。
 * 当后半段比前半段高出 TREND_DELTA (0.5°C) 时视为上升趋势。
 * 至少需要 6 个数据点（30 分钟）才有统计意义。
 */
static int detectRisingTrend(AnalysisRules* rules)
{
    int n = ringSize(&rules->tempHistory);
    if (n < 6) return 0;

    int mid = n / 2;
    double sumFirst = 0, sumSecond = 0;
    for (int i = 0; i < mid; i++)
        sumFirst += ringAt(&rules->tempHistory, i);
    for (int i = mid; i < n; i++)
        sumSecond += ringAt(&rules->tempHistory, i);

    double avgFirst  = sumFirst  / mid;
    double avgSecond = sumSecond / (n - mid);

    return (avgSecond - avgFirst) > TREND_DELTA;
}

/*
 * ── 灌溉用水量计算 ────────────────────────────────────────
 * 用水量 = 流量 (L/min) × 时长 (分钟)。
 * 时长用 GetTickCount64 计算（毫秒精度，不受系统时间调整影响）。
 */
static void accumulateWaterUsage(AnalysisRules* rules)
{
    DWORD64 now = GetTickCount64();
    double elapsedSec = (now - rules->irrigateStartMs) / 1000.0;

    EnterCriticalSection(&rules->configMutex);
    double minutes = elapsedSec / 60.0;
    rules->totalWaterUsed += rules->config.pumpFlowRate * minutes;
    LeaveCriticalSection(&rules->configMutex);
}

/*
 * ── 主分析入口（每 5 分钟调用一次） ──────────────────────
 *
 * 执行顺序：
 *   1. 趋势检测 → 连续 3 次上升则发蜂鸣器 '3'
 *   2. DLI 补光 → 余弦进度法，日出-日落追 DL 目标
 *   3. 温度阈值 → 按作物，超限发蜂鸣器 '3' + 灌溉互锁
 *   4. 土壤湿度 → 按作物 + 高温禁灌，控制水泵 '4'/'6'
 *
 * cmds 数组最多写入 maxCmds 个指令字符，返回实际写入了多少个。
 */
int AnalysisRules_checkUrgent(AnalysisRules* rules, char* cmds, int maxCmds)
{
    int cmdCount = 0;
    if (rules->dataPointCount == 0) return 0;

    /* 取最新一个数据点 */
    double temperature    = ringSize(&rules->tempHistory)  > 0
                            ? ringAt(&rules->tempHistory, ringSize(&rules->tempHistory) - 1)
                            : 0;
    double soilMoisture   = ringSize(&rules->soilHistory)  > 0
                            ? ringAt(&rules->soilHistory, ringSize(&rules->soilHistory) - 1)
                            : 0;
    /* ── 第 1 步：趋势检测 ────────────────── */
    if (detectRisingTrend(rules))
    {
        rules->risingCount++;
        if (rules->risingCount >= TREND_COUNT && !rules->trendAlarmSent)
        {
            if (cmdCount < maxCmds) cmds[cmdCount++] = '3';
            rules->trendAlarmSent = 1;
        }
    }
    else
    {
        rules->risingCount = 0;
        rules->trendAlarmSent = 0;
        rules->trendAlarmActive = 0;
    }

    /* ── 第 2 步：DLI 补光（余弦进度法） ──── */
    int cropIdx;
    {
        EnterCriticalSection(&rules->configMutex);
        cropIdx = (int)rules->config.cropType;
        LeaveCriticalSection(&rules->configMutex);
    }

    double currentDli = AnalysisRules_currentDli(rules);
    double dliTarget  = CROP_DLI[cropIdx];

    time_t now = time(NULL);
    struct tm tmNow;
    localtime_s(&tmNow, &now);
    int curMin = tmNow.tm_hour * 60 + tmNow.tm_min;
    int riseMin = 6 * 60;       /* 日出 6:00 */
    int setMin  = 18 * 60;      /* 日落 18:00 */

    if (curMin < riseMin)
    {
        /* 日出前：强制关补光 */
        if (cmdCount < maxCmds) cmds[cmdCount++] = '5';
    }
    else if (curMin >= setMin)
    {
        /* 日落后：全天 DLI 总量未达标则补，够了就关 */
        if (currentDli < dliTarget)
            { if (cmdCount < maxCmds) cmds[cmdCount++] = '1'; }
        else
            { if (cmdCount < maxCmds) cmds[cmdCount++] = '5'; }
    }
    else
    {
        /* 白天：按余弦进度比较 */
        double t = (double)(curMin - riseMin) / (setMin - riseMin);
        double ratio = 0.5 - 0.5 * cos(M_PI * t);
        double expected = dliTarget * ratio;
        if (currentDli < expected)
            { if (cmdCount < maxCmds) cmds[cmdCount++] = '1'; }
        else
            { if (cmdCount < maxCmds) cmds[cmdCount++] = '5'; }
    }

    /* ── 第 3 步：温度阈值 + 自动释放 ─────── */
    if (temperature > CROP_TMP_MAX[cropIdx])
    {
        rules->tempAlarmActive = 1;
        if (!rules->tempAlarmSent && cmdCount < maxCmds)
        {
            cmds[cmdCount++] = '3';
            rules->tempAlarmSent = 1;
        }
    }
    else if (rules->tempAlarmActive && temperature < CROP_TMP_REL[cropIdx])
    {
        /* 温度回落到释放阈值以下 → 自动清除报警 */
        rules->tempAlarmActive = 0;
        rules->tempAlarmSent   = 0;
    }

    /* ── 第 4 步：土壤湿度 + 高温禁灌 ─────── */
    int irrigateBlocked = (rules->tempAlarmActive ||
                           temperature >= CROP_TMP_IRR[cropIdx]);
    int shouldIrrigate = (soilMoisture < CROP_SOI_DRY[cropIdx] && !irrigateBlocked);
    int shouldStop     = (soilMoisture > CROP_SOI_WET[cropIdx] && rules->irrigating);

    if (shouldIrrigate && !rules->irrigating)
    {
        if (cmdCount < maxCmds) cmds[cmdCount++] = '4';
        rules->irrigating = 1;
        rules->irrigateStartMs = GetTickCount64();
    }

    if (shouldStop)
    {
        if (cmdCount < maxCmds) cmds[cmdCount++] = '6';
        accumulateWaterUsage(rules);
        rules->irrigating = 0;
    }

    return cmdCount;
}

/* 收到 '8' 指令时清除温度报警 */
void AnalysisRules_clearAlarm(AnalysisRules* rules)
{
    rules->tempAlarmActive = 0;
    rules->tempAlarmSent   = 0;
}

/* 切换作物 */
void AnalysisRules_selectCrop(AnalysisRules* rules, CropType crop)
{
    EnterCriticalSection(&rules->configMutex);
    rules->config.cropType = crop;
    LeaveCriticalSection(&rules->configMutex);
}

/* ── 配置读写（线程安全） ───────────────────────────────── */
void AnalysisRules_updateConfig(AnalysisRules* rules, const AnalysisConfig* cfg)
{
    EnterCriticalSection(&rules->configMutex);
    rules->config = *cfg;
    LeaveCriticalSection(&rules->configMutex);
}

AnalysisConfig AnalysisRules_getConfig(const AnalysisRules* rules)
{
    AnalysisConfig cfg;
    /* 注意：const 指针转非 const 以获取锁，数据本身不变 */
    EnterCriticalSection(&((AnalysisRules*)rules)->configMutex);
    cfg = rules->config;
    LeaveCriticalSection(&((AnalysisRules*)rules)->configMutex);
    return cfg;
}

/* ── 只读查询（线程安全说明：字段为 double/int 原子操作，无锁） ── */
double AnalysisRules_totalWaterUsed(const AnalysisRules* rules) { return rules->totalWaterUsed; }
int    AnalysisRules_tempAlarmActive(const AnalysisRules* rules)  { return rules->tempAlarmActive; }

/* PPFD 用全部历史光照数据的平均值乘以转换系数计算 */
double AnalysisRules_avgPpfd(const AnalysisRules* rules)
{
    if (rules->dataPointCount == 0) return 0;
    return (rules->lightSum / rules->dataPointCount) * rules->config.parCoeff;
}

/* DLI = avgPPFD × 光照小时数 × 3600 / 1e6（从累计数据点推算时长） */
double AnalysisRules_currentDli(const AnalysisRules* rules)
{
    double avg = AnalysisRules_avgPpfd(rules);
    if (avg == 0) return 0;
    double hours = rules->dataPointCount * 5.0 / 60.0;     /* 每点 5 分钟 */
    return avg * hours * 3600.0 / 1000000.0;
}

double AnalysisRules_avgTemperature(const AnalysisRules* rules)
{
    return ringAvg(&rules->tempHistory);
}

int AnalysisRules_isIrrigating(const AnalysisRules* rules)    { return rules->irrigating; }
int AnalysisRules_dataPointCount(const AnalysisRules* rules)  { return rules->dataPointCount; }
int AnalysisRules_isTrendAlarm(const AnalysisRules* rules)    { return rules->trendAlarmActive; }
