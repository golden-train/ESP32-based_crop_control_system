#pragma once
#include <SFML/Graphics.hpp>
#include <SFML/Window.hpp>
#include "sensor_data.hpp"
#include <string>
#include <vector>
#include <deque>
#include <queue>
#include <mutex>
#include <functional>

// 页面枚举
enum class Screen
{
    Menu,       // 主菜单
    RealTime,   // 实时数据查看
    Analysis,   // 数据分析
    Manual,     // 手动控制
    Suggestions // 智能建议
};

// SFML 传感器数据显示仪表盘
class SensorDisplay
{
public:
    SensorDisplay();
    ~SensorDisplay();

    void updateData(const SensorData &data);

    // 仅更新当前数值（仪表盘刷新），不修改历史折线图
    void setCurrentData(const SensorData &data);

    // 批量替换全部历史数据（用于从服务端日志重建折线图）
    void setHistory(const double* temp, int tempCount,
                    const double* soil, int soilCount,
                    const double* light, int lightCount);
    void addLog(const std::string &msg);
    void setConnected(bool connected);
    bool processEvents();
    void render();
    bool isOpen() const;

    // 切换到指定页面
    void switchTo(Screen screen);

    // 手动指令队列（数据线程从中取指令）
    static void pushCommand(char cmd);
    static bool popCommand(char &cmd);

    // === 分析数据显示（数据线程调用） ===
    void setAnalysisData(double totalWater, bool alarmActive,
                         double avgPpfd, double dli, double avgTemp,
                         bool irrigating, int dataPoints);
    void setGrowingArea(double area);
    double getGrowingArea() const;

    // === 执行器状态跟踪（数据线程调用） ===
    void applyCommand(char cmd);  // 发送指令后同步更新状态
    bool  isLedOn()    const;
    bool  isPumpOn()   const;
    bool  isBuzzerOn() const;

private:
    // 各页面绘制
    void drawTitleBar();
    void drawMenu();
    void drawRealTime();
    void drawAnalysis();
    void drawManualControl();

    // 子组件
    void drawGauge(float x, float y, float w, float h,
                   const sf::String &label, double value,
                   const sf::String &unit, double min, double max,
                   const sf::Color &barColor);
    void drawHistory(float x, float y, float w, float h,
                     const std::deque<double> &history,
                     double min, double max,
                     const sf::Color &color);
    void drawLogs();
    void drawStatusBar();

    // Manual 页新增面板
    void drawParPanel(float x, float y, float w);
    void drawWaterPanel(float x, float y, float w);
    void drawAlarmPanel(float x, float y, float w);

    // 智能建议页
    void drawSuggestions();

    // 各页共用：底部执行器状态条
    void drawActuatorBar();

    // 按钮辅助
    struct ButtonDef
    {
        sf::FloatRect rect;
        sf::String text;
        std::function<void()> onClick;
    };
    void drawButton(const ButtonDef &btn);
    int hitTest(const std::vector<ButtonDef> &buttons) const;

    // 统计计算
    static double calcMin(const std::deque<double> &d);
    static double calcMax(const std::deque<double> &d);
    static double calcAvg(const std::deque<double> &d);

    static constexpr int HISTORY_SIZE = 300;
    static constexpr int MAX_LOGS = 50;

    sf::RenderWindow m_window;
    sf::Font m_font;
    bool m_fontLoaded = false;

    Screen m_screen = Screen::Menu;
    sf::Vector2i m_mousePos;

    SensorData m_data;
    std::deque<double> m_tempHistory;
    std::deque<double> m_soilHistory;
    std::deque<double> m_lightHistory;
    std::vector<std::string> m_logs;

    std::mutex m_mutex;
    bool m_connected = false;
    sf::Clock m_clock;

    // 分析数据显示（互斥锁保护）
    mutable std::mutex m_analysisMutex;
    double m_displayTotalWater = 0;
    double m_displayAvgPpfd = 0;
    double m_displayDli = 0;
    double m_displayAvgTemp = 0;
    bool   m_displayAlarmActive = false;
    bool   m_displayIrrigating = false;
    int    m_displayDataPoints = 0;
    double m_growingArea = 1.0; // 种植面积 (m²)

    // 执行器状态（数据线程通过 applyCommand 更新）
    bool m_ledOn    = false;
    bool m_pumpOn   = false;
    bool m_buzzerOn = false;

    // 手动指令队列
    static std::queue<char> s_cmdQueue;
    static std::mutex s_cmdMutex;
};
