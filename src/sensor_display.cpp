#include "sensor_display.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <queue>

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
    m_humHistory.push_back(data.humidity);
    m_soilHistory.push_back(data.soilMoisture);
    m_lightHistory.push_back(data.lightIntensity);

    if (m_tempHistory.size() > HISTORY_SIZE) m_tempHistory.pop_front();
    if (m_humHistory.size()   > HISTORY_SIZE) m_humHistory.pop_front();
    if (m_soilHistory.size()  > HISTORY_SIZE) m_soilHistory.pop_front();
    if (m_lightHistory.size() > HISTORY_SIZE) m_lightHistory.pop_front();
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
                // 三个主按钮
                const float bw = 400, bh = 80;
                const float bx = (1280 - bw) / 2;
                const float by1 = 280, by2 = 390, by3 = 500;

                sf::FloatRect r1{bx, by1, bw, bh};
                sf::FloatRect r2{bx, by2, bw, bh};
                sf::FloatRect r3{bx, by3, bw, bh};

                sf::Vector2f mp{(float)m_mousePos.x, (float)m_mousePos.y};
                if (r1.contains(mp)) switchTo(Screen::RealTime);
                if (r2.contains(mp)) switchTo(Screen::Analysis);
                if (r3.contains(mp)) switchTo(Screen::Manual);
                break;
            }
            case Screen::RealTime:
            case Screen::Analysis:
            case Screen::Manual: {
                // "← 返回" 按钮在左上角
                sf::FloatRect backBtn{10, 10, 120, 34};
                sf::Vector2f mp{(float)m_mousePos.x, (float)m_mousePos.y};
                if (backBtn.contains(mp))
                {
                    switchTo(Screen::Menu);
                    break;
                }

                // 手动控制页的特殊按钮
                if (m_screen == Screen::Manual)
                {
                    const float bw = 160, bh = 100;
                    const float gap = 20;
                    const float total = 5 * bw + 4 * gap;
                    const float startX = (1280 - total) / 2;
                    const float btnY = 200;

                    struct { float x; char cmd; } btns[5] = {
                        {startX,              '1'},
                        {startX + (bw+gap),   '2'},
                        {startX + 2*(bw+gap), '3'},
                        {startX + 3*(bw+gap), '4'},
                        {startX + 4*(bw+gap), '9'},
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
        L"", L"实时数据",  // 实时数据
        L"数据分析",         // 数据分析
        L"手动控制"          // 手动控制
    };
    int idx = (int)m_screen;
    if (idx > 0 && idx <= 3)
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

        sf::Text status(m_connected ? L"● COM6 已连接"
                                    : L"● 演示模式",
                        m_font, 14);
        status.setFillColor(m_connected ? sf::Color(80, 220, 80)
                                        : sf::Color(240, 180, 40));
        sf::FloatRect stb = status.getLocalBounds();
        status.setPosition((1280 - stb.width) / 2, 200);
        m_window.draw(status);
    }

    // 三个菜单按钮
    const float bw = 420, bh = 82;
    const float bx = (1280 - bw) / 2;
    const float by[] = {280, 390, 500};

    const wchar_t *labels[] = {
        L"[实时数据]",
        L"[数据分析]",
        L"[手动控制]",
    };
    const wchar_t *descs[] = {
        L"温度、湿度、土壤、光照实时仪表盘",
        L"历史趋势图表与统计分析",
        L"向 ESP32 发送指令控制执行器",
    };

    for (int i = 0; i < 3; i++)
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

        sf::Text desc(descs[i], m_font, 13);
        desc.setFillColor(sf::Color(140, 140, 170));
        desc.setPosition(bx + 24, by[i] + 50);
        m_window.draw(desc);
    }
}

// ── 绘制：实时数据（原有仪表盘） ─────────────────────────
void SensorDisplay::drawRealTime()
{
    drawTitleBar();

    // 拷贝数据
    SensorData data;
    std::deque<double> tHist, hHist, sHist, lHist;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        data  = m_data;
        tHist = m_tempHistory;
        hHist = m_humHistory;
        sHist = m_soilHistory;
        lHist = m_lightHistory;
    }

    const float gaugeW = 240.0f;
    const float gaugeH = 340.0f;
    const float gaugeY = 70.0f;
    const float gap    = 40.0f;
    const float totalW = 4.0f * gaugeW + 3.0f * gap;
    const float startX = (1280.0f - totalW) / 2.0f;

    drawGauge(startX,                    gaugeY, gaugeW, gaugeH,
              L"温度", data.temperature, L"°C", 0, 50,
              sf::Color(230, 80, 80));
    drawGauge(startX + (gaugeW + gap),   gaugeY, gaugeW, gaugeH,
              L"湿度", data.humidity, L"%", 0, 100,
              sf::Color(70, 170, 250));
    drawGauge(startX + 2.0f * (gaugeW + gap), gaugeY, gaugeW, gaugeH,
              L"土壤湿度", data.soilMoisture, L"%", 0, 100,
              sf::Color(110, 210, 80));
    drawGauge(startX + 3.0f * (gaugeW + gap), gaugeY, gaugeW, gaugeH,
              L"光照强度", data.lightIntensity, L"lux", 0, 2000,
              sf::Color(255, 220, 60));

    const float chartY = 430.0f;
    const float chartH = 180.0f;
    const float chartW = gaugeW;

    drawHistory(startX,                    chartY, chartW, chartH,
                tHist, 0, 50, sf::Color(230, 80, 80, 200));
    drawHistory(startX + (gaugeW + gap),   chartY, chartW, chartH,
                hHist, 0, 100, sf::Color(70, 170, 250, 200));
    drawHistory(startX + 2.0f * (gaugeW + gap), chartY, chartW, chartH,
                sHist, 0, 100, sf::Color(110, 210, 80, 200));
    drawHistory(startX + 3.0f * (gaugeW + gap), chartY, chartW, chartH,
                lHist, 0, 2000, sf::Color(255, 220, 60, 200));

    drawLogs();
}

// ── 绘制：数据分析 ────────────────────────────────────────
void SensorDisplay::drawAnalysis()
{
    drawTitleBar();

    std::deque<double> tHist, hHist, sHist, lHist;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        tHist = m_tempHistory;
        hHist = m_humHistory;
        sHist = m_soilHistory;
        lHist = m_lightHistory;
    }

    // 2×2 排列，每个图表更大
    const float pad = 30.0f;
    const float chartW = (1280.0f - pad * 3) / 2.0f;
    const float chartH = 290.0f;
    const float row1Y = 70.0f;
    const float row2Y = row1Y + chartH + pad;

    struct {
        const wchar_t *name;
        const wchar_t *unit;
        std::deque<double> &hist;
        double yMin, yMax;
        sf::Color color;
    } charts[] = {
        {L"温度", L"°C", tHist, 0, 50, sf::Color(230, 80, 80)},
        {L"湿度", L"%",  hHist, 0, 100, sf::Color(70, 170, 250)},
        {L"土壤湿度", L"%",  sHist, 0, 100, sf::Color(110, 210, 80)},
        {L"光照强度", L"lux", lHist, 0, 2000, sf::Color(255, 220, 60)},
    };

    for (int i = 0; i < 4; i++)
    {
        float cx = pad + (i % 2) * (chartW + pad);
        float cy = (i < 2) ? row1Y : row2Y;

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

    // 5 个指令按钮
    struct CmdBtn {
        wchar_t label[20];
        char cmd;
        wchar_t desc[60];
        sf::Color color;
    };
    CmdBtn btns[] = {
        {L"[光照]", '1', L"增加补光",
         sf::Color(220, 180, 50)},
        {L"[湿度]", '2', L"开启加湿器",
         sf::Color(50, 150, 230)},
        {L"[温度]", '3', L"开启散热风扇",
         sf::Color(220, 80, 60)},
        {L"[土壤]", '4', L"开启灌溉系统",
         sf::Color(80, 180, 60)},
        {L"[关闭]", '9', L"停止所有执行器",
         sf::Color(180, 50, 50)},
    };

    const float bw = 180, bh = 110;
    const float gap = 18;
    const float total = 5 * bw + 4 * gap;
    const float startX = (1280 - total) / 2;
    const float btnY = 140;

    for (int i = 0; i < 5; i++)
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

    // 最近指令记录
    sf::Text logTitle(L"最近指令记录", // 最近指令记录
                      m_font, 15);
    logTitle.setStyle(sf::Text::Bold);
    logTitle.setFillColor(sf::Color(180, 180, 200));
    logTitle.setPosition(30, 300);
    m_window.draw(logTitle);

    sf::Text instr(L"点击上方按钮发送指令，"
                   L"指令将通过串口发送给 ESP32",
                   m_font, 13);
    instr.setFillColor(sf::Color(130, 130, 160));
    instr.setPosition(30, 328);
    m_window.draw(instr);

    // 显示最近的手动指令日志
    sf::Text logText(L"", m_font, 13);
    logText.setFillColor(sf::Color(160, 200, 255));

    std::vector<std::string> localLogs;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        localLogs = m_logs;
    }

    float ly = 360;
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
        connected ? L"已连接 — 串口 COM6"
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
    case Screen::Menu:     drawMenu();     break;
    case Screen::RealTime: drawRealTime(); break;
    case Screen::Analysis: drawAnalysis(); break;
    case Screen::Manual:   drawManualControl(); break;
    }

    // 非菜单页 + 非手动页显示状态栏
    if (m_screen == Screen::RealTime)
        drawStatusBar();

    m_window.display();
}
