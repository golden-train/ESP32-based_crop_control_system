#include "analysis_rules.hpp"
#include <algorithm>
#include <cmath>

// 分析逻辑改造方案v4 — 详见计划文档

// 温度超限阈值 (°C)
static constexpr double TEMP_MAX     = 35.0;
// 温度释放迟滞 (°C) — 回落到此值以下才允许恢复灌溉
static constexpr double TEMP_RELEASE = 30.0;
// 趋势判断：后半段均值比前半段高出的最小差值 (°C)
static constexpr double TREND_DELTA  = 0.5;
// 触发趋势报警的连续检测次数
static constexpr int    TREND_COUNT  = 3;

// 土壤湿度阈值 (%)
static constexpr double SOIL_DRY     = 30.0;  // 低于此值开启灌溉
static constexpr double SOIL_WET     = 65.0;  // 高于此值关闭灌溉

// PAR 默认阈值 (μmol/m²/s) — 可通过 AnalysisConfig 覆盖
static constexpr double PAR_MIN_DEF  = 100.0;
static constexpr double PAR_MAX_DEF  = 200.0;

AnalysisRules::AnalysisRules() = default;

// ── 存入一个数据点 ──────────────────────────────────────────
void AnalysisRules::feedDataPoint(const SensorData &data)
{
    m_tempHistory.push_back(data.temperature);
    m_soilHistory.push_back(data.soilMoisture);
    m_lightHistory.push_back(data.lightIntensity);

    if (m_tempHistory.size() > MAX_HISTORY)  m_tempHistory.pop_front();
    if (m_soilHistory.size() > MAX_HISTORY)  m_soilHistory.pop_front();
    if (m_lightHistory.size() > MAX_HISTORY) m_lightHistory.pop_front();

    // PAR 累计
    std::lock_guard<std::mutex> lock(m_configMutex);
    double ppfd = data.lightIntensity * m_config.parCoeff;
    m_ppfdSum += ppfd;
    m_lightSum += data.lightIntensity;
    m_dataPointCount++;
}

// ── 趋势检测 ────────────────────────────────────────────────
bool AnalysisRules::detectRisingTrend()
{
    size_t n = m_tempHistory.size();
    if (n < 6) return false; // 数据太少，判断无意义

    size_t mid = n / 2;
    double sumFirst = 0, sumSecond = 0;
    for (size_t i = 0; i < mid; ++i) sumFirst  += m_tempHistory[i];
    for (size_t i = mid; i < n; ++i) sumSecond += m_tempHistory[i];

    double avgFirst  = sumFirst  / mid;
    double avgSecond = sumSecond / (n - mid);

    return (avgSecond - avgFirst) > TREND_DELTA;
}

// ── 灌溉用水量计算 ──────────────────────────────────────────
void AnalysisRules::accumulateWaterUsage()
{
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - m_irrigateStart).count();

    // 用水量 = 流量 (L/min) × 时长 (分钟)
    std::lock_guard<std::mutex> lock(m_configMutex);
    double minutes = elapsed / 60.0;
    m_totalWaterUsed += m_config.pumpFlowRate * minutes;
}

// ── 主分析入口（每 5 分钟调用） ─────────────────────────────
std::vector<char> AnalysisRules::checkUrgent()
{
    std::vector<char> cmds;

    if (m_dataPointCount == 0) return cmds;

    SensorData data;
    {
        data.temperature    = m_tempHistory.empty()  ? 0 : m_tempHistory.back();
        data.soilMoisture   = m_soilHistory.empty()  ? 0 : m_soilHistory.back();
        data.lightIntensity = m_lightHistory.empty() ? 0 : m_lightHistory.back();
    }

    // 1. 趋势检测（用全部累计数据）
    if (detectRisingTrend())
    {
        m_risingCount++;
        if (m_risingCount >= TREND_COUNT && !m_trendAlarmSent)
        {
            cmds.push_back('3');
            m_trendAlarmSent = true;
        }
    }
    else
    {
        m_risingCount = 0;
        m_trendAlarmSent = false;
        m_trendAlarmActive = false;
    }

    // 2. PAR 控制（用全部累计光照数据计算）
    double avgLux = m_lightSum / m_dataPointCount;
    std::lock_guard<std::mutex> lock(m_configMutex);
    double ppfd = avgLux * m_config.parCoeff;

    if (ppfd < m_config.parMin)
        cmds.push_back('1');
    else if (ppfd > m_config.parMax)
        cmds.push_back('5');

    // 3. 温度阈值 + 互锁
    if (data.temperature > TEMP_MAX)
    {
        m_tempAlarmActive = true;
        if (!m_tempAlarmSent)
        {
            cmds.push_back('3');
            m_tempAlarmSent = true;
        }
    }

    // 4. 土壤湿度 + 温度互锁
    bool shouldIrrigate = (data.soilMoisture < SOIL_DRY && !m_tempAlarmActive);
    bool shouldStop     = (data.soilMoisture > SOIL_WET && m_irrigating);

    if (shouldIrrigate && !m_irrigating)
    {
        cmds.push_back('4');
        m_irrigating = true;
        m_irrigateStart = std::chrono::steady_clock::now();
    }

    if (shouldStop)
    {
        cmds.push_back('6');
        accumulateWaterUsage();
        m_irrigating = false;
    }

    return cmds;
}

// ── 清除温度报警 ────────────────────────────────────────────
void AnalysisRules::clearAlarm()
{
    m_tempAlarmActive = false;
    m_tempAlarmSent   = false;
}

// ── 配置读写 ────────────────────────────────────────────────
void AnalysisRules::updateConfig(const AnalysisConfig &cfg)
{
    std::lock_guard<std::mutex> lock(m_configMutex);
    m_config = cfg;
}

AnalysisConfig AnalysisRules::getConfig() const
{
    std::lock_guard<std::mutex> lock(m_configMutex);
    return m_config;
}

// ── 统计查询 ────────────────────────────────────────────────
double AnalysisRules::avgPpfd() const
{
    if (m_dataPointCount == 0) return 0;
    std::lock_guard<std::mutex> lock(m_configMutex);
    return (m_lightSum / m_dataPointCount) * m_config.parCoeff;
}

double AnalysisRules::currentDli() const
{
    double avg = avgPpfd();
    if (avg == 0) return 0;
    // 从累计数据点推算经过的小时数
    double hours = m_dataPointCount * 5.0 / 60.0; // 每点5分钟
    // DLI = avgPPFD × 秒数 / 1e6
    return avg * hours * 3600.0 / 1000000.0;
}

double AnalysisRules::avgTemperature() const
{
    if (m_tempHistory.empty()) return 0;
    double sum = 0;
    for (double v : m_tempHistory) sum += v;
    return sum / m_tempHistory.size();
}
