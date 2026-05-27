#include "analysis_rules.hpp"
#include <math.h>
#include <time.h>

extern volatile int g_weatherCode;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * ── 作物参数表 ────────────────────────────────────────────
 * 索引：Crop_Lettuce=0, Crop_Strawberry=1
 */
static const double CROP_DLI[]     = { 17, 22 };
static const double CROP_TMP_MAX[] = { 28, 32 };
static const double CROP_TMP_REL[] = { 26, 30 };
static const double CROP_TMP_IRR[] = { 26, 30 };
static const double CROP_SOI_DRY[] = { 22, 25 };
static const double CROP_SOI_WET[] = { 50, 55 };

/* 每小时 DLI 预算比例（6:00-18:00 共 12 小时，余弦曲线分配） */
static const double HOUR_RATIO[] = {
    0.017, 0.050, 0.080, 0.104, 0.121, 0.130,
    0.130, 0.121, 0.104, 0.080, 0.050, 0.017
};

static const double TREND_DELTA  = 0.5;
static const int    TREND_COUNT  = 4;



static void ringPush(RingBuffer* rb, double val)
{
    if (rb->count >= RING_MAX) {
        rb->head = (rb->head + 1) % RING_MAX;
        rb->count--;
    }
    rb->data[rb->tail] = val;
    rb->tail = (rb->tail + 1) % RING_MAX;
    rb->count++;
}//缓冲区

static int ringSize(const RingBuffer* rb) { return rb->count; }

static double ringAt(const RingBuffer* rb, int idx)
{
    return rb->data[(rb->head + idx) % RING_MAX];
}

static double ringAvg(const RingBuffer* rb)
{
    if (rb->count == 0) return 0;
    double sum = 0;
    for (int i = 0; i < rb->count; i++)
        sum += ringAt(rb, i);
    return sum / rb->count;
}


/* 返回当前小时 (6-17) 的 DLI 预算 (mol/m²)，其他小时返回 0 */
static double hourBudget(int hour, double dliTarget)
{
    if (hour < 6 || hour > 17) return 0;
    return dliTarget * HOUR_RATIO[hour - 6];
}

void AnalysisRules_init(AnalysisRules* rules)
{
    rules->tempHistory.head = rules->tempHistory.tail = rules->tempHistory.count = 0;
    rules->soilHistory.head = rules->soilHistory.tail = rules->soilHistory.count = 0;
    rules->lightHistory.head = rules->lightHistory.tail = rules->lightHistory.count = 0;

    rules->risingCount       = 0;
    rules->trendAlarmActive  = 0;
    rules->trendAlarmSent    = 0;

    rules->lightSum       = 0;
    rules->dataPointCount = 0;
    rules->dliAccum       = 0;
    rules->hourAccum      = 0;
    rules->lastPpfd       = 0;
    rules->lastTs         = 0;
    rules->currentHour    = -1;

    rules->irrigating        = 0;
    rules->irrigateStartMs   = 0;
    rules->irrigateStartTime = 0;
    rules->totalWaterUsed    = 0;

    rules->lastDay = -1;

    rules->tempAlarmActive   = 0;
    rules->tempAlarmSent     = 0;

    rules->config.growingArea  = 0.1;
    rules->config.pumpFlowRate = 0.5;
    rules->config.parCoeff     = 0.0185;
    rules->config.cropType     = Crop_Lettuce;

    InitializeCriticalSection(&rules->configMutex);
}

void AnalysisRules_feedDataPoint(AnalysisRules* rules, const SensorData* data, time_t ts)
{
    if (ts == 0) ts = time(NULL);

    struct tm tmNow;
    localtime_s(&tmNow, &ts);
    int today = tmNow.tm_yday;
    int hour  = tmNow.tm_hour;

    /* 跨天重置 */
    if (today != rules->lastDay)
    {
        rules->lightSum       = 0;
        rules->dataPointCount = 0;
        rules->dliAccum       = 0;
        rules->hourAccum      = 0;
        rules->lastPpfd       = 0;
        rules->lastTs         = 0;
        rules->currentHour    = -1;
        rules->lastDay        = today;
    }

    ringPush(&rules->tempHistory, data->temperature);
    ringPush(&rules->soilHistory, data->soilMoisture);
    ringPush(&rules->lightHistory, data->lightIntensity);

    EnterCriticalSection(&rules->configMutex);
    double ppfd = data->lightIntensity * rules->config.parCoeff;
    rules->lightSum += data->lightIntensity;
    rules->dataPointCount++;
    LeaveCriticalSection(&rules->configMutex);

    /* 小时切换时重置 hourAccum */
    if (hour != rules->currentHour)
    {
        rules->hourAccum = 0;
        rules->currentHour = hour;
    }

    /* DLI 积分：用真实时间戳间隔 */
    if (rules->lastTs > 0)
    {
        double dt_sec = difftime(ts, rules->lastTs);
        if (dt_sec > 0 && dt_sec < 7200)
        {
            double inc = rules->lastPpfd * dt_sec;
            rules->dliAccum  += inc;
            rules->hourAccum += inc;
        }
    }
    rules->lastPpfd = ppfd;
    rules->lastTs = ts;
}

/* ── 趋势检测 ────────────────────────────────────────────── */
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

/* ── 用水量 ──────────────────────────────────────────────── */
static void accumulateWaterUsage(AnalysisRules* rules)
{
    double elapsedSec;
    time_t now = time(NULL);
    if (rules->irrigateStartTime > 0) {
        elapsedSec = difftime(now, rules->irrigateStartTime);
    } else {
        elapsedSec = (GetTickCount64() - rules->irrigateStartMs) / 1000.0;
    }
    EnterCriticalSection(&rules->configMutex);
    rules->totalWaterUsed += rules->config.pumpFlowRate * (elapsedSec / 60.0);
    LeaveCriticalSection(&rules->configMutex);
}

/*
 * ── 主分析入口 ─────────────────────────────────────────────
 */
int AnalysisRules_checkUrgent(AnalysisRules* rules, char* cmds, int maxCmds)
{
    int cmdCount = 0;
    if (rules->dataPointCount == 0) return 0;

    double temperature  = ringSize(&rules->tempHistory) > 0
        ? ringAt(&rules->tempHistory, ringSize(&rules->tempHistory) - 1) : 0;
    double soilMoisture = ringSize(&rules->soilHistory) > 0
        ? ringAt(&rules->soilHistory, ringSize(&rules->soilHistory) - 1) : 0;

    /* ── 第 1 步：趋势检测 ── */
    if (detectRisingTrend(rules)) {
        rules->risingCount++;
        if (rules->risingCount >= TREND_COUNT && !rules->trendAlarmSent) {
            if (cmdCount < maxCmds) cmds[cmdCount++] = '3';
            rules->trendAlarmSent = 1;
        }
    } else {
        rules->risingCount = 0;
        rules->trendAlarmSent = 0;
        rules->trendAlarmActive = 0;
    }

    /* ── 第 2 步：DLI 补光（按小时预算） ── */
    int cropIdx;
    { EnterCriticalSection(&rules->configMutex);
      cropIdx = (int)rules->config.cropType;
      LeaveCriticalSection(&rules->configMutex); }

    double dliTarget = CROP_DLI[cropIdx];
    int hour = rules->currentHour;
    if (hour < 0) hour = 0;
    double budget = hourBudget(hour, dliTarget);

    if (hour < 6 || hour >= 22)
    {
        /* 22:00-6:00：强制关灯 */
        if (cmdCount < maxCmds) cmds[cmdCount++] = '5';
    }
    else if (hour >= 6 && hour < 18)
    {
        /* 6:00-18:00：按小时预算 + 天气判断 */
        int isClear = (g_weatherCode >= 0 && g_weatherCode <= 1);
        double hDli = rules->hourAccum / 1000000.0;

        if (isClear && hour < 15)
        {
            if (cmdCount < maxCmds) cmds[cmdCount++] = '5';
        }
        else if (hDli < budget)
        {
            if (cmdCount < maxCmds) cmds[cmdCount++] = '1';
        }
        else
        {
            if (cmdCount < maxCmds) cmds[cmdCount++] = '5';
        }
    }
    else
    {
        /* 18:00-22:00：全天 DLI 未达标则补光 */
        double totalDli = rules->dliAccum / 1000000.0;
        if (totalDli < dliTarget)
        {
            if (cmdCount < maxCmds) cmds[cmdCount++] = '1';
        }
        else
        {
            if (cmdCount < maxCmds) cmds[cmdCount++] = '5';
        }
    }

    /* ── 第 3 步：温度阈值 + 自动释放 ── */
    if (temperature > CROP_TMP_MAX[cropIdx]) {
        rules->tempAlarmActive = 1;
        if (!rules->tempAlarmSent && cmdCount < maxCmds) {
            cmds[cmdCount++] = '3';
            rules->tempAlarmSent = 1;
        }
    } else if (rules->tempAlarmActive && temperature < CROP_TMP_REL[cropIdx]) {
        rules->tempAlarmActive = 0;
        rules->tempAlarmSent   = 0;
    }

//高温处理，灌溉器互锁，防止高温下浇灌
    int irrigateBlocked = (rules->tempAlarmActive ||
                           temperature >= CROP_TMP_IRR[cropIdx]);
    int shouldIrrigate = (soilMoisture < CROP_SOI_DRY[cropIdx] && !irrigateBlocked);
    int shouldStop     = (soilMoisture > CROP_SOI_WET[cropIdx] && rules->irrigating);

    if (shouldIrrigate && !rules->irrigating) {
        if (cmdCount < maxCmds) cmds[cmdCount++] = '4';
        rules->irrigating = 1;
        rules->irrigateStartMs = GetTickCount64();
        rules->irrigateStartTime = time(NULL);
    }
    if (shouldStop) {
        if (cmdCount < maxCmds) cmds[cmdCount++] = '6';
        accumulateWaterUsage(rules);
        rules->irrigating = 0;
    }

    return cmdCount;
}

void AnalysisRules_clearAlarm(AnalysisRules* rules)
{
    rules->tempAlarmActive = 0;
    rules->tempAlarmSent   = 0;
}

void AnalysisRules_selectCrop(AnalysisRules* rules, CropType crop)
{
    EnterCriticalSection(&rules->configMutex);
    rules->config.cropType = crop;
    LeaveCriticalSection(&rules->configMutex);
}

void AnalysisRules_updateConfig(AnalysisRules* rules, const AnalysisConfig* cfg)
{
    EnterCriticalSection(&rules->configMutex);
    rules->config = *cfg;
    LeaveCriticalSection(&rules->configMutex);
}

AnalysisConfig AnalysisRules_getConfig(const AnalysisRules* rules)
{
    AnalysisConfig cfg;
    EnterCriticalSection(&((AnalysisRules*)rules)->configMutex);
    cfg = rules->config;
    LeaveCriticalSection(&((AnalysisRules*)rules)->configMutex);
    return cfg;
}

/* ── 只读查询 ── */
double AnalysisRules_totalWaterUsed(const AnalysisRules* rules) { return rules->totalWaterUsed; }
int    AnalysisRules_tempAlarmActive(const AnalysisRules* rules) { return rules->tempAlarmActive; }

double AnalysisRules_avgPpfd(const AnalysisRules* rules)
{
    if (rules->dataPointCount == 0) return 0;
    return (rules->lightSum / rules->dataPointCount) * rules->config.parCoeff;
}

double AnalysisRules_currentDli(const AnalysisRules* rules)
{
    return rules->dliAccum / 1000000.0;
}

double AnalysisRules_hourDli(const AnalysisRules* rules)
{
    return rules->hourAccum / 1000000.0;
}

double AnalysisRules_avgTemperature(const AnalysisRules* rules)
{
    return ringAvg(&rules->tempHistory);
}

int AnalysisRules_isIrrigating(const AnalysisRules* rules)    { return rules->irrigating; }
int AnalysisRules_dataPointCount(const AnalysisRules* rules)  { return rules->dataPointCount; }
int AnalysisRules_isTrendAlarm(const AnalysisRules* rules)    { return rules->trendAlarmActive; }
