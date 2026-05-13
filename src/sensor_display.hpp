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
    Manual      // 手动控制
};

// SFML 传感器数据显示仪表盘
class SensorDisplay
{
public:
    SensorDisplay();
    ~SensorDisplay();

    void updateData(const SensorData &data);
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
    std::deque<double> m_humHistory;
    std::deque<double> m_soilHistory;
    std::deque<double> m_lightHistory;
    std::vector<std::string> m_logs;

    std::mutex m_mutex;
    bool m_connected = false;
    sf::Clock m_clock;

    // 手动指令队列
    static std::queue<char> s_cmdQueue;
    static std::mutex s_cmdMutex;
};
