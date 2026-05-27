#include "sensor_display.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <queue>
#include <cstring>
#include <cstdio>
#include "system_utils.hpp"

// 各作物空气湿度范围 [作物][低/高]
static const double CROP_HUMID_OK[2][2] = {
    {50, 70},   // 生菜
    {60, 75},   // 草莓
};

// 静态成员
std::queue<char> SensorDisplay::s_cmdQueue;
std::mutex SensorDisplay::s_cmdMutex;

void SensorDisplay::pushCommand(char cmd)
{
    std::lock_guard<std::mutex> lock(s_cmdMutex);
    s_cmdQueue.push(cmd);
}

bool SensorDisplay::popCommand(char &cmd)
{
    std::lock_guard<std::mutex> lock(s_cmdMutex);
    if (s_cmdQueue.empty()) return false;
    cmd = s_cmdQueue.front();
    s_cmdQueue.pop();
    return true;
}

// ── 构造 / 析构 ──────────────────────────────────────────
SensorDisplay::SensorDisplay()
    : m_window(sf::VideoMode(1280, 800),
               L"ESP32 传感器监控系统",
               sf::Style::Titlebar | sf::Style::Close)
{
    const std::vector<std::string> fontPaths = {
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/msyhbd.ttc",
        "C:/Windows/Fonts/Deng.ttf",
        "C:/Windows/Fonts/simfang.ttf",
        "C:/Windows/Fonts/simsun.ttc",
        "C:/Windows/Fonts/arial.ttf",
    };
    std::string loadedFont;
    for (const auto &p : fontPaths)
    {
        if (m_font.loadFromFile(p))
        {
            m_fontLoaded = true;
            loadedFont = p;
            break;
        }
    }
    std::cout << "[显示] 字体 " << (m_fontLoaded ? loadedFont : "加载失败！")
              << std::endl;

    m_window.setFramerateLimit(30);
    m_logs.reserve(MAX_LOGS);
}

SensorDisplay::~SensorDisplay()
{
    if (m_window.isOpen())
        m_window.close();
}

void SensorDisplay::updateData(const SensorData &data)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_data = data;

    m_tempHistory.push_back(data.temperature);
    m_soilHistory.push_back(data.soilMoisture);
    m_lightHistory.push_back(data.lightIntensity);

    if (m_tempHistory.size() > HISTORY_SIZE) m_tempHistory.pop_front();
    if (m_soilHistory.size()  > HISTORY_SIZE) m_soilHistory.pop_front();
    if (m_lightHistory.size() > HISTORY_SIZE) m_lightHistory.pop_front();
}

void SensorDisplay::setCurrentData(const SensorData &data)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_data = data;
}

void SensorDisplay::clearAll()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_data = SensorData();
        m_tempHistory.clear();
        m_soilHistory.clear();
        m_lightHistory.clear();
        m_logs.clear();
        m_connected = false;
    }
    {
        std::lock_guard<std::mutex> lock(m_analysisMutex);
        m_displayTotalWater = 0;
        m_displayAvgPpfd = 0;
        m_displayDli = 0;
        m_displayAvgTemp = 0;
        m_displayAlarmActive = false;
        m_displayIrrigating = false;
        m_displayDataPoints = 0;
    }
    m_ledOn = false;
    m_pumpOn = false;
    m_buzzerOn = false;
}

void SensorDisplay::setHistory(const double* temp, int tempCount,
                                const double* soil, int soilCount,
                                const double* light, int lightCount)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_tempHistory.assign(temp, temp + tempCount);
    m_soilHistory.assign(soil, soil + soilCount);
    m_lightHistory.assign(light, light + lightCount);
}

void SensorDisplay::addLog(const std::string &msg)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_logs.push_back(msg);
    if (m_logs.size() > MAX_LOGS)
        m_logs.erase(m_logs.begin(),
                     m_logs.begin() + (m_logs.size() - MAX_LOGS));
}

void SensorDisplay::setConnected(bool connected)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_connected = connected;
}

void SensorDisplay::switchTo(Screen screen)
{
    m_screen = screen;
}

void SensorDisplay::setAnalysisData(double totalWater, bool alarmActive,
                                     double avgPpfd, double dli, double avgTemp,
                                     bool irrigating, int dataPoints)
{
    std::lock_guard<std::mutex> lock(m_analysisMutex);
    m_displayTotalWater = totalWater;
    m_displayAlarmActive = alarmActive;
    m_displayAvgPpfd = avgPpfd;
    m_displayDli = dli;
    m_displayAvgTemp = avgTemp;
    m_displayIrrigating = irrigating;
    m_displayDataPoints = dataPoints;
}

void SensorDisplay::setGrowingArea(double area)
{
    std::lock_guard<std::mutex> lock(m_analysisMutex);
    m_growingArea = area;
}

double SensorDisplay::getGrowingArea() const
{
    std::lock_guard<std::mutex> lock(m_analysisMutex);
    return m_growingArea;
}

void SensorDisplay::setCropType(int crop)
{
    std::lock_guard<std::mutex> lock(m_analysisMutex);
    m_cropType = crop;
}

int SensorDisplay::getCropType() const
{
    std::lock_guard<std::mutex> lock(m_analysisMutex);
    return m_cropType;
}

bool SensorDisplay::processEvents()
{
    m_mousePos = sf::Mouse::getPosition(m_window);

    sf::Event event;
    while (m_window.pollEvent(event))
    {
        if (event.type == sf::Event::Closed)
        {
            m_window.close();
            return false;
        }

        if (event.type == sf::Event::MouseButtonPressed &&
            event.mouseButton.button == sf::Mouse::Left)
        {
            // 各页面自己的按钮处理
            switch (m_screen)
            {
            case Screen::Menu: {
                // 四个主按钮（坐标须与 drawMenu 一致）
                const float bw = 420, bh = 72;
                const float bx = (1280 - bw) / 2;
                const float by1 = 280, by2 = 368, by3 = 456, by4 = 544;

                sf::FloatRect r1{bx, by1, bw, bh};
                sf::FloatRect r2{bx, by2, bw, bh};
                sf::FloatRect r3{bx, by3, bw, bh};
                sf::FloatRect r4{bx, by4, bw, bh};

                sf::Vector2f mp{(float)m_mousePos.x, (float)m_mousePos.y};
                if (r1.contains(mp)) switchTo(Screen::RealTime);
                if (r2.contains(mp)) switchTo(Screen::Analysis);
                if (r3.contains(mp)) switchTo(Screen::Manual);
                if (r4.contains(mp)) switchTo(Screen::Suggestions);
                // 模式切换按钮
                {
                    sf::FloatRect rMode{30, 660, 160, 32};
                    if (rMode.contains(mp) && !g_modeTogglePending)
                    {
                        g_modeTogglePending = 1;
                        addLog(u8"[模式] 切换请求已发送");
                    }
                }
                break;
            }
            case Screen::RealTime:
            case Screen::Analysis:
            case Screen::Manual:
            case Screen::Suggestions: {
                // "← 返回" 按钮在左上角
                sf::FloatRect backBtn{10, 10, 120, 34};
                sf::Vector2f mp{(float)m_mousePos.x, (float)m_mousePos.y};
                if (backBtn.contains(mp))
                {
                    switchTo(Screen::Menu);
                    break;
                }

                // 实时数据页：立即刷新按钮
                if (m_screen == Screen::RealTime)
                {
                    sf::FloatRect refBtn{1140, 14, 120, 28};
                    if (refBtn.contains(mp))
                    {
                        pushCommand('T');
                        addLog(u8"[触发] 指令 T → 立即上报");
                    }
                }

                // 手动控制页的特殊按钮
                if (m_screen == Screen::Manual)
                {
                    const float bw = 220, bh = 100;
                    const float gap = 30;
                    const float total = 4 * bw + 3 * gap;
                    const float startX = (1280 - total) / 2;
                    const float btnY = 200;

                    struct { float x; char cmd; } btns[4] = {
                        {startX,              '1'},
                        {startX + (bw+gap),   '3'},
                        {startX + 2*(bw+gap), '4'},
                        {startX + 3*(bw+gap), '9'},
                    };
                    for (auto &b : btns)
                    {
                        sf::FloatRect r{b.x, btnY, bw, bh};
                        if (r.contains(mp))
                        {
                            pushCommand(b.cmd);
                            addLog(std::string(u8"[手动] 指令 ") + b.cmd + u8" 已加入队列");
                        }
                    }

                    // 种植面积 [-] [+] 按钮
                    {
                        sf::FloatRect rDec{230, 300, 24, 22};
                        sf::FloatRect rInc{260, 300, 24, 22};
                        if (rDec.contains(mp))
                        {
                            double area = getGrowingArea() - 0.5;
                            if (area < 0.1) area = 0.1;
                            setGrowingArea(area);
                            addLog(std::string(u8"[设置] 种植面积 ") +
                                   std::to_string(area).substr(0, 3) + u8" m²");
                        }
                        if (rInc.contains(mp))
                        {
                            double area = getGrowingArea() + 0.5;
                            if (area > 100) area = 100;
                            setGrowingArea(area);
                            addLog(std::string(u8"[设置] 种植面积 ") +
                                   std::to_string(area).substr(0, 3) + u8" m²");
                        }
                    }

                    // 作物切换按钮
                    {
                        sf::FloatRect rCrop0{90, 374, 66, 22};
                        sf::FloatRect rCrop1{170, 374, 66, 22};
                        if (rCrop0.contains(mp))
                        {
                            setCropType(0);
                            addLog(u8"[设置] 作物: 生菜 (DLI 目标 17 mol/m²/d)");
                        }
                        if (rCrop1.contains(mp))
                        {
                            setCropType(1);
                            addLog(u8"[设置] 作物: 草莓 (DLI 目标 22 mol/m²/d)");
                        }
                    }

                    // 清除报警按钮
                    {
                        sf::FloatRect rClear{480, 502, 130, 30};
                        if (rClear.contains(mp))
                        {
                            pushCommand('8');
                            addLog(u8"[手动] 指令 8 → 清除报警");
                        }
                    }

                    // 建议页清除报警按钮
                    if (m_screen == Screen::Suggestions)
                    {
                        sf::FloatRect rClear{1010, 82, 130, 28};
                        if (rClear.contains(mp))
                        {
                            pushCommand('8');
                            addLog(u8"[建议] 指令 8 → 清除报警");
                        }
                    }
                }
                break;
            }
            }
        }
    }
    return m_window.isOpen();
}

bool SensorDisplay::isOpen() const
{
    return m_window.isOpen();
}

// ── 按钮辅助 ──────────────────────────────────────────────
void SensorDisplay::drawButton(const ButtonDef &btn)
{
    if (!m_fontLoaded) return;

    sf::Vector2f mp{(float)m_mousePos.x, (float)m_mousePos.y};
    bool hover = btn.rect.contains(mp);

    sf::RectangleShape shape({btn.rect.width, btn.rect.height});
    shape.setPosition(btn.rect.left, btn.rect.top);
    shape.setFillColor(hover ? sf::Color(60, 60, 90) : sf::Color(44, 44, 66));
    shape.setOutlineThickness(2);
    shape.setOutlineColor(hover ? sf::Color(120, 180, 255)
                                : sf::Color(70, 70, 100));
    m_window.draw(shape);

    sf::Text txt(btn.text, m_font, 20);
    txt.setFillColor(hover ? sf::Color::White : sf::Color(200, 200, 220));
    sf::FloatRect tb = txt.getLocalBounds();
    txt.setPosition(btn.rect.left + (btn.rect.width  - tb.width) / 2,
                    btn.rect.top  + (btn.rect.height - tb.height) / 2 - 3);
    m_window.draw(txt);
}

int SensorDisplay::hitTest(const std::vector<ButtonDef> &buttons) const
{
    sf::Vector2f mp{(float)m_mousePos.x, (float)m_mousePos.y};
    for (size_t i = 0; i < buttons.size(); ++i)
        if (buttons[i].rect.contains(mp))
            return (int)i;
    return -1;
}

// ── 统计 ──────────────────────────────────────────────────
double SensorDisplay::calcMin(const std::deque<double> &d)
{
    if (d.empty()) return 0;
    double v = d[0];
    for (auto x : d) if (x < v) v = x;
    return v;
}
double SensorDisplay::calcMax(const std::deque<double> &d)
{
    if (d.empty()) return 0;
    double v = d[0];
    for (auto x : d) if (x > v) v = x;
    return v;
}
double SensorDisplay::calcAvg(const std::deque<double> &d)
{
    if (d.empty()) return 0;
    double s = 0;
    for (auto x : d) s += x;
    return s / d.size();
}

// ── 绘制：标题栏 + 返回按钮 ──────────────────────────────
void SensorDisplay::drawTitleBar()
{
    if (!m_fontLoaded) return;

    sf::RectangleShape bar({1280.0f, 50.0f});
    bar.setFillColor(sf::Color(20, 20, 32));
    m_window.draw(bar);

    // 标题
    sf::Text title(L"ESP32 传感器监控系统", m_font, 22);
    title.setStyle(sf::Text::Bold);
    title.setFillColor(sf::Color::White);
    sf::FloatRect tb = title.getLocalBounds();
    title.setPosition((1280.0f - tb.width) / 2.0f, 10.0f);
    m_window.draw(title);

    // 返回按钮（非菜单页时显示）
    if (m_screen != Screen::Menu)
    {
        sf::FloatRect backR{10, 10, 110, 30};
        sf::Vector2f mp{(float)m_mousePos.x, (float)m_mousePos.y};
        bool hov = backR.contains(mp);

        sf::RectangleShape backBtn({110, 30});
        backBtn.setPosition(10, 10);
        backBtn.setFillColor(hov ? sf::Color(60, 60, 85)
                                 : sf::Color(40, 40, 60));
        backBtn.setOutlineThickness(1);
        backBtn.setOutlineColor(sf::Color(80, 80, 110));
        m_window.draw(backBtn);

        sf::Text bt(L"← 返回", m_font, 14);
        bt.setFillColor(hov ? sf::Color::White : sf::Color(180, 180, 200));
        bt.setPosition(18, 15);
        m_window.draw(bt);
    }

    // 页面名称（右上角）
    const wchar_t *pageNames[] = {
        L"", L"实时数据",
        L"数据分析",
        L"手动控制",
        L"智能建议"
    };
    int idx = (int)m_screen;
    if (idx > 0 && idx <= 4)
    {
        sf::Text pg(pageNames[idx], m_font, 14);
        pg.setFillColor(sf::Color(130, 130, 160));
        pg.setPosition(1280 - pg.getLocalBounds().width - 20, 15);
        m_window.draw(pg);
    }
}

// ── 绘制：主菜单 ──────────────────────────────────────────
void SensorDisplay::drawMenu()
{
    drawTitleBar();

    // 副标题
    if (m_fontLoaded)
    {
        sf::Text sub(L"请选择功能模块", // 请选择功能模块
                     m_font, 16);
        sub.setFillColor(sf::Color(150, 150, 180));
        sf::FloatRect sb = sub.getLocalBounds();
        sub.setPosition((1280 - sb.width) / 2, 170);
        m_window.draw(sub);

        sf::Text status(m_connected ? L"● 已连接"
                                    : L"● 演示模式",
                        m_font, 14);
        status.setFillColor(m_connected ? sf::Color(80, 220, 80)
                                        : sf::Color(240, 180, 40));
        sf::FloatRect stb = status.getLocalBounds();
        status.setPosition((1280 - stb.width) / 2, 200);
        m_window.draw(status);

        sf::Text addr(L"8.137.48.225:9090", m_font, 12);
        addr.setFillColor(sf::Color(100, 100, 130));
        sf::FloatRect ab = addr.getLocalBounds();
        addr.setPosition((1280 - ab.width) / 2, 222);
        m_window.draw(addr);

        char wbuf[48];
        snprintf(wbuf, sizeof(wbuf), "今日天气: %s", weatherLabel(g_weatherCode));
        sf::Text wt(sf::String::fromUtf8((const uint8_t*)wbuf,
            (const uint8_t*)wbuf + strlen(wbuf)), m_font, 12);
        wt.setFillColor(sf::Color(130, 200, 240));
        sf::FloatRect wb = wt.getLocalBounds();
        wt.setPosition((1280 - wb.width) / 2, 242);
        m_window.draw(wt);
    }

    // 四个菜单按钮
    const float bw = 420, bh = 72;
    const float bx = (1280 - bw) / 2;
    const float by[] = {280, 368, 456, 544};

    const wchar_t *labels[] = {
        L"[实时数据]",
        L"[数据分析]",
        L"[手动控制]",
        L"[智能建议]",
    };
    const wchar_t *descs[] = {
        L"温度、土壤湿度、光照实时仪表盘",
        L"历史趋势图表与统计分析",
        L"向 ESP32 发送指令控制执行器",
        L"基于传感器数据的操作建议",
    };

    for (int i = 0; i < 4; i++)
    {
        sf::FloatRect rect{bx, by[i], bw, bh};
        sf::Vector2f mp{(float)m_mousePos.x, (float)m_mousePos.y};
        bool hover = rect.contains(mp);

        sf::RectangleShape btn({bw, bh});
        btn.setPosition(bx, by[i]);
        btn.setFillColor(hover ? sf::Color(55, 55, 85) : sf::Color(40, 40, 60));
        btn.setOutlineThickness(2);
        btn.setOutlineColor(hover ? sf::Color(100, 160, 255)
                                  : sf::Color(60, 60, 90));
        m_window.draw(btn);

        sf::Text label(labels[i], m_font, 20);
        label.setFillColor(hover ? sf::Color::White : sf::Color(200, 200, 220));
        label.setPosition(bx + 20, by[i] + 14);
        m_window.draw(label);

        sf::Text desc(descs[i], m_font, 12);
        desc.setFillColor(sf::Color(140, 140, 170));
        desc.setPosition(bx + 24, by[i] + 44);
        m_window.draw(desc);
    }

    /* 模式切换按钮（左下角） */
    {
        int isDemo = g_currentMode;
        sf::FloatRect rBtn{30, 660, 160, 32};
        sf::Vector2f mp{(float)m_mousePos.x, (float)m_mousePos.y};
        bool hover = rBtn.contains(mp);

        sf::RectangleShape btn({160, 32});
        btn.setPosition(30, 660);
        btn.setFillColor(hover ? sf::Color(60, 60, 90) : sf::Color(40, 40, 65));
        btn.setOutlineThickness(1);
        btn.setOutlineColor(isDemo ? sf::Color(100, 200, 100) : sf::Color(200, 160, 60));
        m_window.draw(btn);

        char buf[32];
        snprintf(buf, sizeof(buf), "切换: %s", isDemo ? "演示→服务端" : "服务端→演示");
        sf::Text txt(sf::String::fromUtf8((const uint8_t*)buf, (const uint8_t*)buf + strlen(buf)), m_font, 13);
        txt.setFillColor(hover ? sf::Color::White : sf::Color(180, 180, 200));
        sf::FloatRect tb = txt.getLocalBounds();
        txt.setPosition(30 + (160 - tb.width) / 2, 666);
        m_window.draw(txt);
    }
}

// ── 绘制：实时数据（原有仪表盘） ─────────────────────────
void SensorDisplay::drawRealTime()
{
    drawTitleBar();

    // 拷贝数据
    SensorData data;
    std::deque<double> tHist, sHist, lHist;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        data  = m_data;
        tHist = m_tempHistory;
        sHist = m_soilHistory;
        lHist = m_lightHistory;
    }

    const float gaugeW = 300.0f;
    const float gaugeH = 340.0f;
    const float gaugeY = 70.0f;
    const float gap    = 50.0f;
    const float totalW = 3.0f * gaugeW + 2.0f * gap;
    const float startX = (1280.0f - totalW) / 2.0f;

    drawGauge(startX,                    gaugeY, gaugeW, gaugeH,
              L"温度", data.temperature, L"°C", 0, 50,
              sf::Color(230, 80, 80));
    drawGauge(startX + (gaugeW + gap),   gaugeY, gaugeW, gaugeH,
              L"土壤湿度", data.soilMoisture, L"%", 0, 100,
              sf::Color(110, 210, 80));
    drawGauge(startX + 2.0f * (gaugeW + gap), gaugeY, gaugeW, gaugeH,
              L"光照强度", data.lightIntensity, L"lux", 0, 120000,
              sf::Color(255, 220, 60));

    const float chartY = 430.0f;
    const float chartH = 180.0f;
    const float chartW = gaugeW;

    drawHistory(startX,                    chartY, chartW, chartH,
                tHist, 0, 50, sf::Color(230, 80, 80, 200));
    drawHistory(startX + (gaugeW + gap),   chartY, chartW, chartH,
                sHist, 0, 100, sf::Color(110, 210, 80, 200));
    drawHistory(startX + 2.0f * (gaugeW + gap), chartY, chartW, chartH,
                lHist, 0, 120000, sf::Color(255, 220, 60, 200));

    /* "↻ 立即刷新" 按钮 ── 发送 T 指令触发 ESP32 立即上报 */
    {
        sf::Vector2f mp{(float)m_mousePos.x, (float)m_mousePos.y};
        sf::FloatRect refBtn{1140, 14, 120, 28};
        bool hov = refBtn.contains(mp);

        sf::RectangleShape btn({120, 28});
        btn.setPosition(1140, 14);
        btn.setFillColor(hov ? sf::Color(60, 130, 60) : sf::Color(35, 80, 35));
        btn.setOutlineThickness(1);
        btn.setOutlineColor(hov ? sf::Color(100, 210, 100) : sf::Color(50, 100, 50));
        m_window.draw(btn);

        sf::Text txt(L"\x21bb 立即刷新", m_font, 13);
        txt.setFillColor(hov ? sf::Color::White : sf::Color(180, 220, 180));
        sf::FloatRect tb = txt.getLocalBounds();
        txt.setPosition(1140 + (120 - tb.width) / 2, 16);
        m_window.draw(txt);
    }

    drawLogs();
}

// ── 绘制：数据分析 ────────────────────────────────────────
void SensorDisplay::drawAnalysis()
{
    drawTitleBar();

    std::deque<double> tHist, sHist, lHist;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        tHist = m_tempHistory;
        sHist = m_soilHistory;
        lHist = m_lightHistory;
    }

    // 3 图表横向排列
    const float pad = 30.0f;
    const float chartW = (1280.0f - pad * 4) / 3.0f;
    const float chartH = 320.0f;
    const float rowY = 70.0f;

    struct {
        const wchar_t *name;
        const wchar_t *unit;
        std::deque<double> &hist;
        double yMin, yMax;
        sf::Color color;
    } charts[] = {
        {L"温度", L"°C", tHist, 0, 50, sf::Color(230, 80, 80)},
        {L"土壤湿度", L"%",  sHist, 0, 100, sf::Color(110, 210, 80)},
        {L"光照强度", L"lux", lHist, 0, 120000, sf::Color(255, 220, 60)},
    };

    for (int i = 0; i < 3; i++)
    {
        float cx = pad + i * (chartW + pad);
        float cy = rowY;

        // 背景面板
        sf::RectangleShape panel({chartW, chartH});
        panel.setPosition(cx, cy);
        panel.setFillColor(sf::Color(30, 30, 46));
        panel.setOutlineThickness(1);
        panel.setOutlineColor(sf::Color(50, 50, 72));
        m_window.draw(panel);

        // 标题
        if (m_fontLoaded)
        {
            sf::Text title(charts[i].name, m_font, 16);
            title.setStyle(sf::Text::Bold);
            title.setFillColor(sf::Color(200, 200, 220));
            title.setPosition(cx + 12, cy + 8);
            m_window.draw(title);
        }

        // 图表区域
        float plotX = cx + 12;
        float plotY = cy + 36;
        float plotW = chartW - 24;
        float plotH = chartH - 80;

        sf::RectangleShape plotBg({plotW, plotH});
        plotBg.setPosition(plotX, plotY);
        plotBg.setFillColor(sf::Color(20, 20, 34));
        m_window.draw(plotBg);

        // 水平参考线
        for (int g = 0; g < 4; g++)
        {
            float gy = plotY + plotH * (g + 1) / 5.0f;
            sf::Vertex gridLine[] = {
                {{plotX, gy}, sf::Color(40, 40, 58)},
                {{plotX + plotW, gy}, sf::Color(40, 40, 58)},
            };
            m_window.draw(gridLine, 2, sf::Lines);
        }

        // 绘制折线（填满整个图表）
        auto &hist = charts[i].hist;
        double yMin = charts[i].yMin;
        double yMax = charts[i].yMax;
        double range = yMax - yMin;
        sf::Color col = charts[i].color;

        if (hist.size() >= 2 && range > 0.001)
        {
            size_t N = hist.size();
            // 下采样：如果数据点太多，取 plotW 个点
            size_t step = (N > (size_t)plotW) ? (N / (size_t)plotW) : 1;
            size_t nPoints = (N + step - 1) / step;

            std::vector<sf::Vertex> verts;
            verts.reserve(nPoints);
            for (size_t j = 0; j < N; j += step)
            {
                float px = plotX + (float)j / (float)(N - 1) * plotW;
                float vy = (float)((hist[j] - yMin) / range);
                float py = plotY + plotH - vy * plotH;
                verts.push_back({{px, py}, col});
            }
            // 确保最后一个点被画到
            if ((N - 1) % step != 0)
            {
                float px = plotX + plotW;
                float vy = (float)((hist[N - 1] - yMin) / range);
                float py = plotY + plotH - vy * plotH;
                verts.push_back({{px, py}, col});
            }

            if (verts.size() >= 2)
                m_window.draw(verts.data(), verts.size(), sf::LineStrip);
        }

        // 统计信息
        if (m_fontLoaded)
        {
            double mn = calcMin(hist);
            double mx = calcMax(hist);
            double av = calcAvg(hist);

            std::wstringstream ss;
            ss << std::fixed << std::setprecision(1);
            ss << L"最小: " << mn << L"  最大: " << mx
               << L"  平均: " << av;  // 最小 最大 平均

            sf::Text stats(ss.str(), m_font, 13);
            stats.setFillColor(sf::Color(150, 150, 180));
            stats.setPosition(cx + 12, cy + chartH - 32);
            m_window.draw(stats);
        }
    }
}

// ── 绘制：手动控制 ────────────────────────────────────────
void SensorDisplay::drawManualControl()
{
    drawTitleBar();

    if (!m_fontLoaded) return;

    // 大标题
    sf::Text heading(L"发送指令到 ESP32", // 发送指令到 ESP32
                     m_font, 20);
    heading.setStyle(sf::Text::Bold);
    heading.setFillColor(sf::Color(200, 200, 220));
    sf::FloatRect hb = heading.getLocalBounds();
    heading.setPosition((1280 - hb.width) / 2, 80);
    m_window.draw(heading);

    // 4 个指令按钮（湿度=土壤湿度，已合并到[土壤]）
    struct CmdBtn {
        wchar_t label[20];
        char cmd;
        wchar_t desc[60];
        sf::Color color;
    };
    CmdBtn btns[] = {
        {L"[光照]", '1', L"开启 / 关闭补光",
         sf::Color(220, 180, 50)},
        {L"[温度]", '3', L"散热风扇 / 蜂鸣器",
         sf::Color(220, 80, 60)},
        {L"[土壤]", '4', L"开启灌溉系统",
         sf::Color(80, 180, 60)},
        {L"[关闭]", '9', L"停止所有执行器",
         sf::Color(180, 50, 50)},
    };

    const float bw = 220, bh = 110;
    const float gap = 30;
    const float total = 4 * bw + 3 * gap;
    const float startX = (1280 - total) / 2;
    const float btnY = 140;

    for (int i = 0; i < 4; i++)
    {
        float bx = startX + i * (bw + gap);
        sf::FloatRect rect{bx, btnY, bw, bh};
        sf::Vector2f mp{(float)m_mousePos.x, (float)m_mousePos.y};
        bool hover = rect.contains(mp);

        sf::RectangleShape btn({bw, bh});
        btn.setPosition(bx, btnY);
        btn.setFillColor(hover ? btns[i].color : sf::Color(36, 36, 56));
        btn.setOutlineThickness(2);
        btn.setOutlineColor(hover ? btns[i].color : sf::Color(70, 70, 100));
        m_window.draw(btn);

        // 指令名称
        sf::Text label(btns[i].label, m_font, 16);
        label.setStyle(sf::Text::Bold);
        label.setFillColor(hover ? sf::Color::White : sf::Color(200, 200, 220));
        sf::FloatRect lb = label.getLocalBounds();
        label.setPosition(bx + (bw - lb.width) / 2, btnY + 18);
        m_window.draw(label);

        // 指令字符
        std::wstring cmdStr = std::wstring(L"[") + (wchar_t)(btns[i].cmd) + L"]";
        sf::Text cmdText(cmdStr, m_font, 24);
        cmdText.setStyle(sf::Text::Bold);
        cmdText.setFillColor(hover ? btns[i].color : sf::Color(120, 120, 150));
        sf::FloatRect cb = cmdText.getLocalBounds();
        cmdText.setPosition(bx + (bw - cb.width) / 2, btnY + 46);
        m_window.draw(cmdText);

        // 描述
        sf::Text desc(btns[i].desc, m_font, 11);
        desc.setFillColor(sf::Color(140, 140, 170));
        sf::FloatRect db = desc.getLocalBounds();
        desc.setPosition(bx + (bw - db.width) / 2, btnY + 82);
        m_window.draw(desc);
    }

    // ── 三个分析面板 ────────────────────────────
    drawParPanel(30, 270, 1220);
    drawWaterPanel(30, 408, 1220);
    drawAlarmPanel(30, 496, 1220);

    // 最近指令记录
    sf::Text logTitle(L"最近指令记录",
                      m_font, 13);
    logTitle.setStyle(sf::Text::Bold);
    logTitle.setFillColor(sf::Color(160, 160, 180));
    logTitle.setPosition(30, 585);
    m_window.draw(logTitle);

    // 显示最近的手动指令日志
    sf::Text logText(L"", m_font, 12);
    logText.setFillColor(sf::Color(150, 180, 220));

    std::vector<std::string> localLogs;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        localLogs = m_logs;
    }

    float ly = 608;
    int shown = 0;
    for (int i = (int)localLogs.size() - 1; i >= 0 && shown < 8; --i, ++shown)
    {
        auto sfmlStr = sf::String::fromUtf8(
            localLogs[i].begin(), localLogs[i].end());
        logText.setString(sfmlStr);
        logText.setPosition(40, ly);
        m_window.draw(logText);
        ly += 22;
    }
}

// ── 面板：PAR + 种植面积 ──────────────────────────────
void SensorDisplay::drawParPanel(float x, float y, float w)
{
    // 读分析数据
    double growingArea, avgPpfd, dli, avgTemp;
    int dataPoints, cropType;
    {
        std::lock_guard<std::mutex> lock(m_analysisMutex);
        growingArea = m_growingArea;
        avgPpfd     = m_displayAvgPpfd;
        dli         = m_displayDli;
        avgTemp     = m_displayAvgTemp;
        dataPoints  = m_displayDataPoints;
        cropType    = m_cropType;
    }

    sf::RectangleShape bg({w, 130});
    bg.setPosition(x, y);
    bg.setFillColor(sf::Color(28, 28, 44));
    bg.setOutlineThickness(1);
    bg.setOutlineColor(sf::Color(50, 50, 70));
    m_window.draw(bg);

    // 标题
    sf::Text title(L"光照 & PAR", m_font, 14);
    title.setStyle(sf::Text::Bold);
    title.setFillColor(sf::Color(200, 200, 220));
    title.setPosition(x + 12, y + 6);
    m_window.draw(title);

    // 种植面积 [+]/[-] 按钮
    sf::Text areaLabel(L"种植面积:", m_font, 13);
    areaLabel.setFillColor(sf::Color(140, 140, 170));
    areaLabel.setPosition(x + 12, y + 32);
    m_window.draw(areaLabel);

    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f m²", growingArea);
    sf::Text areaVal(sf::String::fromUtf8((const uint8_t*)buf, (const uint8_t*)buf + strlen(buf)), m_font, 13);
    areaVal.setFillColor(sf::Color::White);
    areaVal.setPosition(x + 110, y + 32);
    m_window.draw(areaVal);

    // [-] 按钮
    sf::RectangleShape btnDec({24, 22});
    btnDec.setPosition(x + 200, y + 30);
    btnDec.setFillColor(sf::Color(50, 50, 72));
    btnDec.setOutlineThickness(1);
    btnDec.setOutlineColor(sf::Color(80, 80, 110));
    m_window.draw(btnDec);
    sf::Text decTxt(L"-", m_font, 16);
    decTxt.setFillColor(sf::Color::White);
    decTxt.setPosition(x + 210, y + 30);
    m_window.draw(decTxt);

    // [+] 按钮
    sf::RectangleShape btnInc({24, 22});
    btnInc.setPosition(x + 230, y + 30);
    btnInc.setFillColor(sf::Color(50, 50, 72));
    btnInc.setOutlineThickness(1);
    btnInc.setOutlineColor(sf::Color(80, 80, 110));
    m_window.draw(btnInc);
    sf::Text incTxt(L"+", m_font, 16);
    incTxt.setFillColor(sf::Color::White);
    incTxt.setPosition(x + 240, y + 30);
    m_window.draw(incTxt);

    // PPFD
    snprintf(buf, sizeof(buf), "PPFD: %.1f \xb5mol/m\xb2/s", avgPpfd);
    sf::Text ppfdTxt(sf::String::fromUtf8((const uint8_t*)buf, (const uint8_t*)buf + strlen(buf)), m_font, 13);
    ppfdTxt.setFillColor(sf::Color(180, 220, 100));
    ppfdTxt.setPosition(x + 12, y + 58);
    m_window.draw(ppfdTxt);

    // DLI
    snprintf(buf, sizeof(buf), "DLI: %.1f mol/m\xb2", dli);
    sf::Text dliTxt(sf::String::fromUtf8((const uint8_t*)buf, (const uint8_t*)buf + strlen(buf)), m_font, 13);
    dliTxt.setFillColor(sf::Color(100, 200, 220));
    dliTxt.setPosition(x + 280, y + 58);
    m_window.draw(dliTxt);

    // 数据点计数
    snprintf(buf, sizeof(buf), "数据点: %d (每5分钟)", dataPoints);
    sf::Text countTxt(sf::String::fromUtf8((const uint8_t*)buf, (const uint8_t*)buf + strlen(buf)), m_font, 12);
    countTxt.setFillColor(sf::Color(120, 120, 150));
    countTxt.setPosition(x + 12, y + 82);
    m_window.draw(countTxt);

    // 平均温度
    snprintf(buf, sizeof(buf), "平均温度: %.1f \x0d\x0ac", avgTemp);
    // Use simple °C instead
    snprintf(buf, sizeof(buf), "平均温度: %.1f C", avgTemp);
    sf::Text avgTempTxt(sf::String::fromUtf8((const uint8_t*)buf, (const uint8_t*)buf + strlen(buf)), m_font, 12);
    avgTempTxt.setFillColor(sf::Color(120, 120, 150));
    avgTempTxt.setPosition(x + 280, y + 82);
    m_window.draw(avgTempTxt);

    // ── 作物选择按钮 ──
    const wchar_t *cropLabels[] = { L"生菜", L"草莓" };
    sf::Text cropLabel(L"作物:", m_font, 13);
    cropLabel.setFillColor(sf::Color(140, 140, 170));
    cropLabel.setPosition(x + 12, y + 106);
    m_window.draw(cropLabel);

    for (int i = 0; i < 2; i++)
    {
        float bx = x + 60 + i * 80;
        sf::FloatRect rect{bx, y + 104, 66, 22};
        sf::Vector2f mp{(float)m_mousePos.x, (float)m_mousePos.y};
        bool hover = rect.contains(mp);
        bool selected = (i == cropType);

        sf::RectangleShape btn({66, 22});
        btn.setPosition(bx, y + 104);
        btn.setFillColor(selected ? sf::Color(80, 160, 80) :
                         hover ? sf::Color(55, 55, 85) : sf::Color(40, 40, 60));
        btn.setOutlineThickness(1);
        btn.setOutlineColor(selected ? sf::Color(120, 220, 120) :
                            hover ? sf::Color(100, 100, 140) : sf::Color(60, 60, 90));
        m_window.draw(btn);

        sf::Text txt(cropLabels[i], m_font, 12);
        txt.setFillColor(selected ? sf::Color::White : sf::Color(170, 170, 190));
        sf::FloatRect tb = txt.getLocalBounds();
        txt.setPosition(bx + (66 - tb.width) / 2, y + 106);
        m_window.draw(txt);
    }
}

// ── 面板：用水量 ──────────────────────────────────────
void SensorDisplay::drawWaterPanel(float x, float y, float w)
{
    double totalWater;
    bool irrigating;
    {
        std::lock_guard<std::mutex> lock(m_analysisMutex);
        totalWater  = m_displayTotalWater;
        irrigating = m_displayIrrigating;
    }

    sf::RectangleShape bg({w, 80});
    bg.setPosition(x, y);
    bg.setFillColor(sf::Color(24, 24, 40));
    bg.setOutlineThickness(1);
    bg.setOutlineColor(sf::Color(50, 50, 70));
    m_window.draw(bg);

    sf::Text title(L"灌溉 & 用水量", m_font, 14);
    title.setStyle(sf::Text::Bold);
    title.setFillColor(sf::Color(200, 200, 220));
    title.setPosition(x + 12, y + 6);
    m_window.draw(title);

    char buf[64];
    snprintf(buf, sizeof(buf), "累计用水量: %.1f L", totalWater);
    sf::Text waterTxt(sf::String::fromUtf8((const uint8_t*)buf, (const uint8_t*)buf + strlen(buf)), m_font, 14);
    waterTxt.setFillColor(sf::Color(100, 200, 255));
    waterTxt.setPosition(x + 180, y + 8);
    m_window.draw(waterTxt);

    sf::Text statusTxt(irrigating ? L"灌溉状态: ● 运行中" : L"灌溉状态: ○ 已停止", m_font, 13);
    statusTxt.setFillColor(irrigating ? sf::Color(80, 220, 80) : sf::Color(160, 160, 180));
    statusTxt.setPosition(x + 180, y + 34);
    m_window.draw(statusTxt);

    sf::Text hint(L"土壤 < 55% 开启 | > 73% 关闭 | [温度报警中]禁止灌溉", m_font, 11);
    hint.setFillColor(sf::Color(120, 120, 150));
    hint.setPosition(x + 180, y + 56);
    m_window.draw(hint);
}

// ── 面板：报警状态 + 清除按钮 ──────────────────────────
void SensorDisplay::drawAlarmPanel(float x, float y, float w)
{
    bool alarmActive;
    {
        std::lock_guard<std::mutex> lock(m_analysisMutex);
        alarmActive = m_displayAlarmActive;
    }

    sf::RectangleShape bg({w, 80});
    bg.setPosition(x, y);
    bg.setFillColor(sf::Color(28, 24, 24));
    bg.setOutlineThickness(1);
    bg.setOutlineColor(alarmActive ? sf::Color(180, 50, 50) : sf::Color(50, 50, 70));
    m_window.draw(bg);

    sf::Text title(L"报警状态", m_font, 14);
    title.setStyle(sf::Text::Bold);
    title.setPosition(x + 10, y + 6);
    title.setFillColor(alarmActive ? sf::Color(255, 80, 60) : sf::Color(160, 160, 180));
    m_window.draw(title);

    sf::Text status(alarmActive ? L"[报警] 温度报警激活中 — 灌溉已禁止"
                                : L"[正常] 无报警", m_font, 14);
    status.setFillColor(alarmActive ? sf::Color(255, 100, 80) : sf::Color(140, 200, 140));
    status.setPosition(x + 120, y + 8);
    m_window.draw(status);

    // 清除报警按钮
    sf::FloatRect btnRect{x + 450, y + 6, 130, 30};
    sf::Vector2f mp{(float)m_mousePos.x, (float)m_mousePos.y};
    bool hover = btnRect.contains(mp);

    sf::RectangleShape btn({130, 30});
    btn.setPosition(x + 450, y + 6);
    btn.setFillColor(hover ? sf::Color(160, 50, 50) : sf::Color(120, 40, 40));
    btn.setOutlineThickness(1);
    btn.setOutlineColor(sf::Color(200, 80, 60));
    m_window.draw(btn);

    sf::Text clearTxt(L"我已知晓 [8]", m_font, 13);
    clearTxt.setStyle(sf::Text::Bold);
    clearTxt.setFillColor(hover ? sf::Color::White : sf::Color(220, 180, 180));
    clearTxt.setPosition(x + 458, y + 9);
    m_window.draw(clearTxt);
}

// ── 绘制：智能建议 ─────────────────────────────────────────
void SensorDisplay::drawSuggestions()
{
    drawTitleBar();

    if (!m_fontLoaded) return;

    // 拷贝实时数据 + 分析数据
    SensorData data;
    double totalWater, avgPpfd, dli, avgTemp;
    bool alarmActive, irrigating;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        data = m_data;
    }
    int cropType;
    {
        std::lock_guard<std::mutex> lock(m_analysisMutex);
        totalWater  = m_displayTotalWater;
        avgPpfd     = m_displayAvgPpfd;
        dli         = m_displayDli;
        avgTemp     = m_displayAvgTemp;
        alarmActive = m_displayAlarmActive;
        irrigating = m_displayIrrigating;
        cropType    = m_cropType;
    }

    const float leftX = 40.0f;
    const float panelW = 580.0f;
    const float rightX = 660.0f;
    const float colW = 560.0f;

    // ── 左列：指标状态 + 建议 ─────────────────────────
    sf::RectangleShape leftPanel({panelW, 640.0f});
    leftPanel.setPosition(leftX, 70.0f);
    leftPanel.setFillColor(sf::Color(28, 28, 44));
    leftPanel.setOutlineThickness(1);
    leftPanel.setOutlineColor(sf::Color(50, 50, 70));
    m_window.draw(leftPanel);

    sf::Text leftTitle(L"指标状态与建议", m_font, 16);
    leftTitle.setStyle(sf::Text::Bold);
    leftTitle.setFillColor(sf::Color(200, 200, 220));
    leftTitle.setPosition(leftX + 14, 80);
    m_window.draw(leftTitle);

    // 逐项判断
    double hLow = CROP_HUMID_OK[cropType][0];
    double hHigh = CROP_HUMID_OK[cropType][1];
    struct {
        const char *label;
        double value;
        const char *unit;
        bool ok;
        const char *advice;
        sf::Color okColor;
    } items[5] = {
        {u8"土壤湿度", data.soilMoisture, u8"%",
         data.soilMoisture >= 30 && data.soilMoisture <= 65,
         data.soilMoisture < 30 ? u8"建议开启灌溉" : u8"建议关闭灌溉或等待回落",
         sf::Color(110, 210, 80)},

        {u8"温度", data.temperature, u8"°C",
         !alarmActive,
         u8"温度过高！灌溉已禁止，请检查散热",
         sf::Color(230, 80, 80)},

        {u8"空气湿度", data.humidity, u8"%",
         data.humidity >= hLow && data.humidity <= hHigh,
         data.humidity < hLow ? u8"湿度过低，建议加湿" : u8"湿度过高，建议通风",
         sf::Color(100, 200, 220)},

        {u8"光照", data.lightIntensity, u8"lux",
         data.lightIntensity >= 50,
         u8"光照偏低，建议开启补光",
         sf::Color(255, 220, 60)},

        {u8"温度趋势", 0, u8"",
         m_displayDataPoints < 6,
         u8"温度持续上升，建议检查通风",
         sf::Color(200, 160, 80)},
    };

        int nItems = 5;
    float iy = 110;
    for (int i = 0; i < nItems; i++)
    {
        char buf[128];
        if (i == 4)
        {
            // 趋势行特殊处理
            items[4].ok = (m_displayDataPoints < 6);
            snprintf(buf, sizeof(buf), "数据点: %d", m_displayDataPoints);
        }
        else
        {
            snprintf(buf, sizeof(buf), "%s: %.1f%s",
                     items[i].label, items[i].value, items[i].unit);
        }

        sf::Text valTxt(sf::String::fromUtf8(
            (const uint8_t*)buf, (const uint8_t*)buf + strlen(buf)), m_font, 14);
        valTxt.setFillColor(items[i].ok ? sf::Color(180, 200, 180) : sf::Color(220, 160, 140));
        valTxt.setPosition(leftX + 20, iy);
        m_window.draw(valTxt);

        // 状态指示
        sf::Text status(items[i].ok ? L"[正常]" : L"[异常]", m_font, 14);
        status.setFillColor(items[i].ok ? sf::Color(80, 200, 80) : sf::Color(240, 180, 40));
        status.setPosition(leftX + 340, iy);
        m_window.draw(status);

        if (!items[i].ok)
        {
            sf::Text advise(sf::String::fromUtf8(
                (const uint8_t*)items[i].advice,
                (const uint8_t*)items[i].advice + strlen(items[i].advice)), m_font, 12);
            advise.setFillColor(sf::Color(200, 180, 100));
            advise.setPosition(leftX + 24, iy + 22);
            m_window.draw(advise);
        }

        iy += 58;
    }

    // ── 右列 ─────────────────────────────────────────
    // 区块2：报警状态
    sf::RectangleShape alarmPanel({colW, 140.0f});
    alarmPanel.setPosition(rightX, 70.0f);
    alarmPanel.setFillColor(sf::Color(28, 24, 24));
    alarmPanel.setOutlineThickness(1);
    alarmPanel.setOutlineColor(alarmActive ? sf::Color(180, 50, 50) : sf::Color(50, 50, 70));
    m_window.draw(alarmPanel);

    sf::Text alarmTitle(alarmActive ? L"[报警状态]" : L"[无报警]", m_font, 15);
    alarmTitle.setStyle(sf::Text::Bold);
    alarmTitle.setFillColor(alarmActive ? sf::Color(255, 80, 60) : sf::Color(140, 200, 140));
    alarmTitle.setPosition(rightX + 14, 80);
    m_window.draw(alarmTitle);

    if (alarmActive)
    {
        sf::Text alarmDesc(u8"温度报警激活中 — 灌溉已禁止", m_font, 13);
        alarmDesc.setFillColor(sf::Color(220, 160, 140));
        alarmDesc.setPosition(rightX + 18, 108);
        m_window.draw(alarmDesc);

        sf::Text alarmHint(u8"请检查温度状况，确认正常后点击下方清除报警", m_font, 11);
        alarmHint.setFillColor(sf::Color(150, 130, 120));
        alarmHint.setPosition(rightX + 18, 130);
        m_window.draw(alarmHint);

        // 清除按钮
        sf::FloatRect btnRect{rightX + 350, 82, 130, 28};
        sf::Vector2f mp{(float)m_mousePos.x, (float)m_mousePos.y};
        bool hover = btnRect.contains(mp);

        sf::RectangleShape btn({130, 28});
        btn.setPosition(rightX + 350, 82);
        btn.setFillColor(hover ? sf::Color(160, 50, 50) : sf::Color(120, 40, 40));
        btn.setOutlineThickness(1);
        btn.setOutlineColor(sf::Color(200, 80, 60));
        m_window.draw(btn);
        sf::Text btnTxt(L"我已知晓 [8]", m_font, 13);
        btnTxt.setStyle(sf::Text::Bold);
        btnTxt.setFillColor(hover ? sf::Color::White : sf::Color(220, 180, 180));
        btnTxt.setPosition(rightX + 358, 85);
        m_window.draw(btnTxt);
    }

    // 区块3：统计摘要
    sf::RectangleShape statsPanel({colW, 260.0f});
    statsPanel.setPosition(rightX, 230.0f);
    statsPanel.setFillColor(sf::Color(28, 28, 44));
    statsPanel.setOutlineThickness(1);
    statsPanel.setOutlineColor(sf::Color(50, 50, 70));
    m_window.draw(statsPanel);

    sf::Text statsTitle(L"统计摘要", m_font, 15);
    statsTitle.setStyle(sf::Text::Bold);
    statsTitle.setFillColor(sf::Color(200, 200, 220));
    statsTitle.setPosition(rightX + 14, 240);
    m_window.draw(statsTitle);

    char buf[128];
    int sy = 272;
    struct { const char *fmt; double v; const char *unit; } stats[] = {
        {u8"累计用水量: %.1f %s",  totalWater, u8"L"},
        {u8"全程平均温度: %.1f %s", avgTemp,   u8"°C"},
        {u8"平均 PPFD: %.1f %s",   avgPpfd,   u8"μmol/m²/s"},
        {u8"累计 DLI: %.1f %s",    dli,        u8"mol/m²"},
    };
    for (auto &s : stats)
    {
        snprintf(buf, sizeof(buf), s.fmt, s.v, s.unit);
        sf::Text line(sf::String::fromUtf8(
            (const uint8_t*)buf, (const uint8_t*)buf + strlen(buf)), m_font, 13);
        line.setFillColor(sf::Color(150, 170, 190));
        line.setPosition(rightX + 18, sy);
        m_window.draw(line);
        sy += 32;
    }

    // 灌溉状态
    sf::Text irrTxt(irrigating ? L"灌溉状态: ● 运行中" : L"灌溉状态: ○ 已停止", m_font, 13);
    irrTxt.setFillColor(irrigating ? sf::Color(80, 220, 80) : sf::Color(160, 160, 180));
    irrTxt.setPosition(rightX + 18, sy + 8);
    m_window.draw(irrTxt);

    // 区块4：天气种植建议
    {
        int wc = g_weatherCode;
        const char* wlabel = weatherLabel(wc);
        const char* wadvice;
        if (wc < 0) wadvice = "无法获取天气数据，系统按保守策略运行";
        else if (wc <= 1) wadvice = "日照充足，注意温度过高触发报警，建议午后通风";
        else if (wc <= 3) wadvice = "云量较多，自然光照偏弱，建议关注DLI进度";
        else if (wc == 45 || wc == 48) wadvice = "有雾，湿度偏高，注意通风防病";
        else if (wc <= 65) wadvice = "雨天，光照不足湿度偏高，建议补光并注意通风";
        else if (wc <= 82) wadvice = "阵雨天气，光照波动大，系统按需补光";
        else wadvice = "天气不稳定，建议关注传感器数据";

        sf::RectangleShape panel({560, 110});
        panel.setPosition(660, 520);
        panel.setFillColor(sf::Color(24, 28, 36));
        panel.setOutlineThickness(1);
        panel.setOutlineColor(sf::Color(60, 80, 100));
        m_window.draw(panel);

        sf::Text title(L"今日种植建议", m_font, 14);
        title.setStyle(sf::Text::Bold);
        title.setFillColor(sf::Color(160, 200, 240));
        title.setPosition(674, 528);
        m_window.draw(title);

        char wbuf[64];
        snprintf(wbuf, sizeof(wbuf), "天气: %s", wlabel);
        sf::Text line1(sf::String::fromUtf8(
            (const uint8_t*)wbuf, (const uint8_t*)wbuf + strlen(wbuf)), m_font, 13);
        line1.setFillColor(sf::Color(150, 190, 220));
        line1.setPosition(678, 552);
        m_window.draw(line1);

        sf::Text line2(sf::String::fromUtf8(
            (const uint8_t*)wadvice, (const uint8_t*)wadvice + strlen(wadvice)), m_font, 13);
        line2.setFillColor(sf::Color(180, 200, 200));
        line2.setPosition(678, 578);
        m_window.draw(line2);
    }
}

// ── 底部执行器状态条（所有页面共用） ──────────────────────
void SensorDisplay::drawActuatorBar()
{
    if (!m_fontLoaded) return;

    sf::RectangleShape bar({1280.0f, 28.0f});
    bar.setPosition(0.0f, 772.0f);
    bar.setFillColor(sf::Color(12, 12, 20));
    m_window.draw(bar);

    sf::Text prefix(L"执行器:", m_font, 12);
    prefix.setFillColor(sf::Color(130, 130, 160));
    prefix.setPosition(14.0f, 778.0f);
    m_window.draw(prefix);

    struct { const char *label; bool on; sf::Color onColor; } act[] = {
        {u8"LED",   m_ledOn,    sf::Color(255, 220, 60)},
        {u8"水泵",  m_pumpOn,   sf::Color(80, 180, 255)},
        {u8"蜂鸣器", m_buzzerOn, sf::Color(255, 80, 60)},
    };

    float cx = 90.0f;
    for (auto &a : act)
    {
        sf::CircleShape dot(5.0f);
        dot.setFillColor(a.on ? a.onColor : sf::Color(60, 60, 70));
        dot.setPosition(cx, 781.0f);
        m_window.draw(dot);

        sf::Text label(sf::String::fromUtf8(
            (const uint8_t*)a.label,
            (const uint8_t*)a.label + strlen(a.label)), m_font, 12);
        label.setFillColor(a.on ? sf::Color(200, 200, 220) : sf::Color(90, 90, 110));
        label.setPosition(cx + 12.0f, 778.0f);
        m_window.draw(label);
        cx += 80.0f;
    }

    // 右侧：连接状态 + 页面
    sf::Text status(
        m_connected ? L"已连接" : L"演示模式",
        m_font, 12);
    status.setFillColor(m_connected ? sf::Color(80, 220, 80) : sf::Color(240, 180, 40));
    status.setPosition(1280.0f - status.getLocalBounds().width - 100.0f, 778.0f);
    m_window.draw(status);

    char pageBuf[32];
    snprintf(pageBuf, sizeof(pageBuf), "页面: %d", (int)m_screen);
    sf::Text pageTxt(sf::String::fromUtf8(
        (const uint8_t*)pageBuf,
        (const uint8_t*)pageBuf + strlen(pageBuf)), m_font, 11);
    pageTxt.setFillColor(sf::Color(100, 100, 120));
    pageTxt.setPosition(1280.0f - pageTxt.getLocalBounds().width - 14.0f, 778.0f);
    m_window.draw(pageTxt);
}

// ── 执行器状态同步 ──────────────────────────────────────
void SensorDisplay::applyCommand(char cmd)
{
    switch (cmd)
    {
    case '1': m_ledOn    = true;  break;
    case '5': m_ledOn    = false; break;
    case '2': m_pumpOn   = true;  break;
    case '4': m_pumpOn   = true;  break;
    case '6': m_pumpOn   = false; break;
    case '3': m_buzzerOn = true;  break;
    case '8': m_buzzerOn = false; break;
    case '9': m_ledOn = m_pumpOn = m_buzzerOn = false; break;
    }
}

bool SensorDisplay::isLedOn()    const { return m_ledOn; }
bool SensorDisplay::isPumpOn()   const { return m_pumpOn; }
bool SensorDisplay::isBuzzerOn() const { return m_buzzerOn; }

// ── 绘制：单个仪表 ────────────────────────────────────────
void SensorDisplay::drawGauge(float x, float y, float w, float h,
                               const sf::String &label, double value,
                               const sf::String &unit,
                               double min, double max,
                               const sf::Color &barColor)
{
    const float PAD = 12.0f;

    sf::RectangleShape panel({w, h});
    panel.setPosition(x, y);
    panel.setFillColor(sf::Color(36, 36, 52));
    panel.setOutlineThickness(1.0f);
    panel.setOutlineColor(sf::Color(60, 60, 80));
    m_window.draw(panel);

    const float innerY  = y + PAD;
    const float innerH  = h - PAD * 2.0f;
    const float barW    = w - PAD * 2.0f;
    const float barTopY = innerY + 36.0f;
    const float barH    = innerH - 36.0f - 30.0f;

    if (m_fontLoaded)
    {
        sf::Text lb(label, m_font, 16);
        lb.setStyle(sf::Text::Bold);
        lb.setFillColor(sf::Color(200, 200, 220));
        sf::FloatRect lbb = lb.getLocalBounds();
        lb.setPosition(x + (w - lbb.width) / 2.0f, innerY + 4.0f);
        m_window.draw(lb);
    }

    sf::RectangleShape track({barW, barH});
    track.setPosition(x + PAD, barTopY);
    track.setFillColor(sf::Color(20, 20, 32));
    m_window.draw(track);

    double ratio = (value - min) / (max - min);
    ratio = std::max(0.0, std::min(1.0, ratio));
    float fillH = static_cast<float>(ratio * barH);

    if (fillH > 1.0f)
    {
        sf::RectangleShape fill({barW - 4.0f, fillH});
        fill.setPosition(x + PAD + 2.0f, barTopY + barH - fillH);
        fill.setFillColor(barColor);
        m_window.draw(fill);
    }

    if (m_fontLoaded)
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << value;
        sf::Text valText(oss.str(), m_font, 28);
        valText.setStyle(sf::Text::Bold);
        valText.setFillColor(sf::Color::White);
        sf::FloatRect vb = valText.getLocalBounds();
        valText.setPosition(x + (w - vb.width) / 2.0f,
                            barTopY + barH + 2.0f);
        m_window.draw(valText);

        sf::Text unitText(unit, m_font, 14);
        unitText.setFillColor(sf::Color(160, 160, 180));
        sf::FloatRect ub = unitText.getLocalBounds();
        unitText.setPosition(x + (w - ub.width) / 2.0f,
                             barTopY + barH + 34.0f);
        m_window.draw(unitText);

        {
            std::ostringstream ms;
            ms << std::fixed << std::setprecision(0) << min;
            sf::Text minTxt(ms.str(), m_font, 11);
            minTxt.setFillColor(sf::Color(120, 120, 140));
            minTxt.setPosition(x + PAD + 2.0f, barTopY + barH - 16.0f);
            m_window.draw(minTxt);
        }
        {
            std::ostringstream ms;
            ms << std::fixed << std::setprecision(0) << max;
            sf::Text maxTxt(ms.str(), m_font, 11);
            maxTxt.setFillColor(sf::Color(120, 120, 140));
            maxTxt.setPosition(x + PAD + 2.0f, barTopY + 2.0f);
            m_window.draw(maxTxt);
        }
    }
}

// ── 绘制：趋势折线图 ──────────────────────────────────────
void SensorDisplay::drawHistory(float x, float y, float w, float h,
                                 const std::deque<double> &history,
                                 double min, double max,
                                 const sf::Color &color)
{
    sf::RectangleShape panel({w, h});
    panel.setPosition(x, y);
    panel.setFillColor(sf::Color(28, 28, 44));
    panel.setOutlineThickness(1.0f);
    panel.setOutlineColor(sf::Color(50, 50, 70));
    m_window.draw(panel);

    if (history.size() < 2) return;

    const float padX = 4.0f, padY = 4.0f;
    const float plotW = w - padX * 2.0f;
    const float plotH = h - padY * 2.0f;
    const float range = static_cast<float>(max - min);
    if (range < 0.001f) return;

    std::vector<sf::Vertex> vertices;
    vertices.reserve(history.size());

    const size_t N = history.size();
    for (size_t i = 0; i < N; ++i)
    {
        float px = x + padX + (static_cast<float>(i) / (N - 1)) * plotW;
        float vy = static_cast<float>((history[i] - min) / range);
        float py = y + padY + plotH - vy * plotH;
        vertices.push_back(sf::Vertex({px, py}, color));
    }

    std::vector<sf::Vertex> fillVerts;
    fillVerts.reserve((N + 2) * 2);
    for (size_t i = 0; i < N; ++i)
    {
        float px = x + padX + (static_cast<float>(i) / (N - 1)) * plotW;
        float vy = static_cast<float>((history[i] - min) / range);
        float py = y + padY + plotH - vy * plotH;
        fillVerts.push_back({{px, py}, color});
        fillVerts.push_back({{px, y + padY + plotH},
                             sf::Color(color.r, color.g, color.b, 40)});
    }

    if (fillVerts.size() >= 4)
        m_window.draw(fillVerts.data(), fillVerts.size(), sf::TrianglesStrip);

    if (vertices.size() >= 2)
        m_window.draw(vertices.data(), vertices.size(), sf::LineStrip);
}

// ── 绘制：日志区 ──────────────────────────────────────────
void SensorDisplay::drawLogs()
{
    if (!m_fontLoaded) return;

    const float logY = 625.0f;
    const float logH = 140.0f;

    sf::RectangleShape bg({1280.0f, logH});
    bg.setPosition(0.0f, logY);
    bg.setFillColor(sf::Color(18, 18, 28));
    m_window.draw(bg);

    sf::Text heading(L"—— 运行日志 ——", m_font, 13);
    heading.setFillColor(sf::Color(130, 130, 150));
    heading.setPosition(16.0f, logY + 6.0f);
    m_window.draw(heading);

    sf::Text lineTxt("", m_font, 13);
    lineTxt.setFillColor(sf::Color(180, 180, 200));

    std::vector<std::string> localLogs;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        localLogs = m_logs;
    }

    float lineY = logY + 30.0f;
    int shown = 0;
    for (int i = static_cast<int>(localLogs.size()) - 1;
         i >= 0 && shown < 6; --i, ++shown)
    {
        auto sfmlStr = sf::String::fromUtf8(
            localLogs[i].begin(), localLogs[i].end());
        lineTxt.setString(sfmlStr);
        lineTxt.setPosition(20.0f, lineY);
        m_window.draw(lineTxt);
        lineY += 18.0f;
    }
}

// ── 绘制：状态栏 ──────────────────────────────────────────
void SensorDisplay::drawStatusBar()
{
    if (!m_fontLoaded) return;

    bool connected;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        connected = m_connected;
    }

    sf::RectangleShape bar({1280.0f, 28.0f});
    bar.setPosition(0.0f, 772.0f);
    bar.setFillColor(sf::Color(12, 12, 20));
    m_window.draw(bar);

    sf::CircleShape dot(5.0f);
    dot.setPosition(14.0f, 783.0f);
    sf::Color indicatorColor = connected
        ? sf::Color(80, 220, 80)
        : sf::Color(240, 180, 40);
    dot.setFillColor(indicatorColor);
    m_window.draw(dot);

    sf::Text status(
        connected ? L"已连接 — 服务端"
                  : L"演示模式 — 模拟数据",
        m_font, 13);
    status.setFillColor(sf::Color(160, 160, 180));
    status.setPosition(26.0f, 777.0f);
    m_window.draw(status);

    sf::Text fps(L"当前页面: " +
                 std::to_wstring((int)m_screen), m_font, 12);
    fps.setFillColor(sf::Color(100, 100, 120));
    fps.setPosition(1280.0f - fps.getLocalBounds().width - 16.0f, 777.0f);
    m_window.draw(fps);
}

// ── 主渲染入口 ────────────────────────────────────────────
void SensorDisplay::render()
{
    m_window.clear(sf::Color(24, 24, 36));

    switch (m_screen)
    {
    case Screen::Menu:        drawMenu();        break;
    case Screen::RealTime:    drawRealTime();    break;
    case Screen::Analysis:    drawAnalysis();    break;
    case Screen::Manual:      drawManualControl(); break;
    case Screen::Suggestions: drawSuggestions(); break;
    }

    // 全局报警横幅（所有页面顶部可见）
    bool alarmActive;
    {
        std::lock_guard<std::mutex> lock(m_analysisMutex);
        alarmActive = m_displayAlarmActive;
    }
    if (alarmActive && m_fontLoaded)
    {
        sf::RectangleShape banner({1280, 34});
        banner.setFillColor(sf::Color(200, 40, 40, 230));
        banner.setPosition(0, 50);
        m_window.draw(banner);

        sf::Text warnTxt(L"[!] 温度报警 — 灌溉已禁止，请在 [手动控制] 页清除报警", m_font, 14);
        warnTxt.setStyle(sf::Text::Bold);
        warnTxt.setFillColor(sf::Color::White);
        warnTxt.setPosition(20, 54);
        m_window.draw(warnTxt);
    }

    // 所有页面底部显示执行器状态条 + 连接状态
    drawActuatorBar();

    m_window.display();
}
