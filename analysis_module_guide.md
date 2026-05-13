# 分析模块接入说明

这份文档说明当前项目中"数据分析"模块的实现方式，以及如何扩展它。

---

## 当前项目结构

```
src/
├── sensor_data.hpp          # 传感器数据结构（4 个 double）
├── number_parser.hpp/.cpp   # 文本 → 数字提取
├── analysis_rules.hpp/.cpp  # 异常规则判定
├── data_logger.hpp/.cpp     # 日志记录
├── simulated_sensor.hpp/.cpp# 模拟数据生成
├── serial_reader.hpp/.cpp   # 串口读写（Windows API）
├── serial_dispatcher.hpp/.cpp# 串口后台读取 + 分发
├── sensor_display.hpp/.cpp  # SFML 图形界面（4 页面）
└── system.cpp               # 程序入口 + 数据采集主循环
```

---

## 数据流

```
ESP32 / 模拟传感器
    │
    ▼
extractNumbers(line)           ← number_parser.hpp
    │  返回 vector<double> {温度, 湿度, 土壤湿度, 光照}
    │
    ├──→ processData(data)     ← 控制台输出 + 日志 + UI 更新
    │
    └──→ AnalysisRules::observe(values)
              │
              ├── 正常 → 无动作
              └── 异常 → 返回指令列表 {'1','2','3','4'}
                         → writeText(cmd) → ESP32 执行器
    │
    ▼
SensorDisplay::updateData(data)  ← SFML 仪表盘实时更新
```

---

## 核心分析接口

### `extractNumbers()` — 数字提取

```cpp
// 文件: src/number_parser.hpp
#include "number_parser.hpp"

// 从任意文本中提取所有浮点数
// 输入: "25.3 55.1 42.7 813" 或 "ERR 38.5 60 42 800"
// 输出: {25.3, 55.1, 42.7, 813.0}
std::vector<double> extractNumbers(const std::string &line);
```

### `AnalysisRules` — 异常规则

```cpp
// 文件: src/analysis_rules.hpp
#include "analysis_rules.hpp"

AnalysisRules rules;
std::vector<char> cmds = rules.observe(values);
```

#### 当前阈值表

定义在 `analysis_rules.cpp`，可通过修改宏调整：

| 传感器 | 正常范围 | 越限指令 | 含义 |
|--------|----------|----------|------|
| 温度 | 0–45°C | `'3'` | 开启散热风扇 |
| 湿度 | 10–90% | `'2'` | 开启加湿器 |
| 土壤湿度 | 10–90% | `'4'` | 开启灌溉系统 |
| 光照强度 | 5–95% | `'1'` | 增加补光 |

指令 `'9'` 用于关闭所有执行器（异常恢复正常时发送）。

#### 调用方式

```cpp
#include "number_parser.hpp"
#include "analysis_rules.hpp"

void handleLine(const std::string &line)
{
    // 1. 提取数字
    const std::vector<double> values = extractNumbers(line);

    // 2. 规则判定
    AnalysisRules rules;
    std::vector<char> commands = rules.observe(values);

    // 3. 发送指令
    for (char cmd : commands)
    {
        // 通过串口发送: '1'=补光, '2'=加湿, '3'=散热, '4'=灌溉
        reader.writeText(std::string(1, cmd), sendError);
    }
}
```

---

## 数据分析页（UI）

### 页面位置

数据分析页是 `SensorDisplay` 的 4 个页面之一：

```cpp
enum class Screen { Menu, RealTime, Analysis, Manual };
```

### 当前展示内容

`drawAnalysis()` 方法（`sensor_display.cpp:418-549`）渲染 2×2 大图表：

| 位置 | 传感器 | 量程 | 颜色 |
|------|--------|------|------|
| 左上 | 温度 | 0–50°C | 红色 |
| 右上 | 湿度 | 0–100% | 蓝色 |
| 左下 | 土壤湿度 | 0–100% | 绿色 |
| 右下 | 光照强度 | 0–2000 lux | 黄色 |

每个图表包含：
- 标题标签
- 水平参考线（4 条网格线）
- 历史折线图（从 `SensorDisplay` 内部缓冲区读取，最多 300 点）
- 统计信息：**最小 / 最大 / 平均** 值

### 历史数据的存储

数据存在 `SensorDisplay` 的私有成员中：

```cpp
// sensor_display.hpp
std::deque<double> m_tempHistory;   // 最多 HISTORY_SIZE(300) 个点
std::deque<double> m_humHistory;
std::deque<double> m_soilHistory;
std::deque<double> m_lightHistory;
```

由 `updateData()` 方法（数据线程调用）推送，`drawAnalysis()` 读取并绘制。

---

## 如何扩展分析模块

### 方案一：修改阈值

直接改 `analysis_rules.cpp` 顶部的宏定义即可：

```cpp
#define TEMP_MAX     45.0    // 改成你想要的上限
#define HUMI_MIN     10.0    // 改成你想要的下限
```

### 方案二：增加新规则

在 `AnalysisRules::observe()` 中添加新的判断逻辑：

```cpp
std::vector<char> AnalysisRules::observe(const std::vector<double> &values)
{
    std::vector<char> cmds;
    double temp = values[0], humi = values[1];
    double soil = values[2], light = values[3];

    // 已有规则
    if (light < LIGHT_MIN || light > LIGHT_MAX) cmds.push_back('1');
    if (humi  < HUMI_MIN  || humi  > HUMI_MAX ) cmds.push_back('2');
    if (temp  < TEMP_MIN  || temp  > TEMP_MAX ) cmds.push_back('3');
    if (soil  < SOIL_MIN  || soil  > SOIL_MAX ) cmds.push_back('4');

    // === 在这里添加你的新规则 ===
    // 例如：温差过大报警
    // if (temp > 40 && humi < 20) cmds.push_back('5');

    return cmds;
}
```

注意：新指令需要 ESP32 端也有对应的处理逻辑。

### 方案三：增加统计指标到 UI

数据分析页的统计信息目前显示最小/最大/平均，由 `SensorDisplay::calcMin/Max/Avg()` 计算。

如果要增加新指标（如中位数、标准差、变化率），步骤：

1. 在 `sensor_display.hpp` 中添加统计方法：
```cpp
static double calcMedian(const std::deque<double> &d);
```

2. 在 `sensor_display.cpp` 中实现，并在 `drawAnalysis()` 的统计信息输出中调用。

### 方案四：新增独立分析类

当分析逻辑变得复杂时（如趋势预测、FFT、机器学习），建议新增独立文件：

```
src/trend_analysis.hpp/.cpp    # 趋势分析
src/alarm_analysis.hpp/.cpp    # 报警规则
```

然后在 `system.cpp` 的数据线程中调用：

```cpp
#include "trend_analysis.hpp"

TrendAnalysis trend;
// 在主循环中
trend.feed(values);
std::vector<std::string> report = trend.buildReport();
```

---

## 实际调用位置

### 数据入口

`system.cpp` 的 `processData()` 函数（约第 51 行）：

```cpp
static void processData(const SensorData &data)
{
    // 控制台输出
    // DataLogger::log()  写入日志文件
    // SensorDisplay::updateData() 更新 UI
}
```

该函数被 `runRealMode()` 和 `runDemoMode()` 每帧调用。

### 规则分析入口

`system.cpp` 的 `runRealMode()` 约第 192–222 行，`runDemoMode()` 约第 273–301 行：

```cpp
AnalysisRules rules;
std::vector<char> cmds = rules.observe(fileValues);
// 根据 cmds 发送指令到 ESP32
```

### UI 渲染入口

每帧（约 60fps）调用 `SystemDisplay::render()` → 根据当前 `m_screen` 派发到对应页面的 `draw*()` 方法。
