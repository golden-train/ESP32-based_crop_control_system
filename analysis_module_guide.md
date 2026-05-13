# 分析模块接入说明

这份文档说明如何在现有项目里继续手写“数据分析”模块，并把它接到 UI 中。

## 现在的调用链

当前程序的数据流是：

1. [src/serial_reader.cpp](src/serial_reader.cpp) 负责从串口读取完整文本行。
2. [src/number_parser.cpp](src/number_parser.cpp) 把文本行提取成数字序列。
3. [src/reading_analysis.cpp](src/reading_analysis.cpp) 负责累计统计结果。
4. [src/system.cpp](src/system.cpp) 把统计结果打包成 [src/monitor_ui.hpp](src/monitor_ui.hpp) 需要的视图数据。
5. [src/monitor_ui.cpp](src/monitor_ui.cpp) 负责把页面画出来。

## 现在已经提供的分析能力

目前 `ReadingAnalysis` 已经具备这些基础能力：

- 统计总行数、有效行数、无效行数。
- 统计前三个通道的最小值、最大值、平均值、最新值。
- 生成可直接显示到 UI 的中文文本行。

你可以把第 0/1/2 个数分别理解为湿度、温度、光强，也可以以后改成别的业务含义。

### 规则触发分析

除了基础统计之外，当前项目还额外增加了一个专门的规则分析模块：

- 温度 > 35，或者温度 < 15，触发蜂鸣器，向开发板发送 `1`
- 温度连续 3 次升高，触发蜂鸣器，向开发板发送 `1`
- 湿度 < 30%，触发灌溉，向开发板发送 `2`
- 光照 < 400，触发开灯，向开发板发送 `3`
- 光照恢复正常后，向开发板发送 `6` 关闭灯

这部分逻辑由 [src/analysis_rules.hpp](src/analysis_rules.hpp) 和 [src/analysis_rules.cpp](src/analysis_rules.cpp) 提供。

这个模块的核心入口是 `AnalysisRules::observe()`：

1. 传入当前行提取出来的数字数组。
2. 它会内部判断是否满足蜂鸣器、灌溉、开灯的条件。
3. 如果触发，就返回需要发送回串口的指令列表，例如 `1`、`2`、`3`。
4. UI 侧则读取 `buildReportLines()` 的结果，把触发状态显示出来。

如果这一行没有触发任何条件，`buildReportLines()` 可以返回空数组，分析页就不需要额外显示结果。

## 如果要手写扩展分析模块，建议怎么做

### 方案一：继续扩展 `ReadingAnalysis`

适合你想在现有结构上直接加功能，比如：

- 阈值报警
- 趋势判断
- 滑动窗口平均值
- 最大最小差值
- 某个通道超限次数

建议做法：

1. 在 [src/reading_analysis.hpp](src/reading_analysis.hpp) 里给 `ReadingAnalysis` 增加新的统计字段。
2. 在 [src/reading_analysis.cpp](src/reading_analysis.cpp) 里实现对应逻辑。
3. 在 `buildReportLines()` 里把新结果转成 UI 能直接显示的中文行。
4. 在 [src/system.cpp](src/system.cpp) 的 `buildMonitorViewState()` 中把这些文本放进 `MonitorViewState`。
5. 在 [src/monitor_ui.cpp](src/monitor_ui.cpp) 的数据页里增加一块新的面板显示这些结果。

### 方案二：单独新增一个分析类

如果后续分析逻辑会明显变复杂，比如要做多种算法、历史曲线、导出文件，建议新增一个独立分析器，例如：

- `TrendAnalysis`
- `AlarmAnalysis`
- `HistoryAnalysis`

这样 `ReadingAnalysis` 继续保持“基础统计”，新类负责“高级分析”。

## 如何调用分析模块

你现在可以直接在主循环里按这个模式使用：

1. 先把串口一行文本交给 `extractNumbers()`。
2. 再把返回的 `std::vector<double>` 传给 `ReadingAnalysis::observe()`。
3. 最后调用 `ReadingAnalysis::buildReportLines()`，拿到适合界面显示的文本。

核心伪代码如下：

```cpp
const std::vector<double> values = extractNumbers(line);
analysis.observe(values);
const std::vector<std::string> reportLines = analysis.buildReportLines();
```

### 最小接入范例

如果你要在自己的代码里直接调用，可以按照下面的顺序写。

```cpp
#include "number_parser.hpp"
#include "reading_analysis.hpp"
#include "analysis_rules.hpp"

ReadingAnalysis analysis;
AnalysisRules rules;

void handleLine(const std::string& line)
{
	const std::vector<double> values = extractNumbers(line);
	analysis.observe(values);

	const std::vector<char> commands = rules.observe(values);
	for (char command : commands)
	{
		// 把 command 通过串口写回开发板，例如 "1" / "2" / "3"
	}

	const std::vector<std::string> reportLines = analysis.buildReportLines();
	for (const auto& reportLine : reportLines)
	{
		std::cout << reportLine << std::endl;
	}
}
```

这个例子里最关键的是两步：

1. 先调用 `extractNumbers()`，把字符串变成数字列表。
2. 再调用 `ReadingAnalysis::observe()`，把这一行数据喂给分析器。

如果你要做规则报警，再额外调用 `AnalysisRules::observe()`，它会返回本次新触发的指令列表。

如果你不想直接打印，也可以把 `reportLines` 继续传给 UI 或写进文件。

### 在主循环里放哪里

推荐把分析调用放在“收到完整一行数据之后”的位置，也就是：

1. 串口读到一行完整文本。
2. 立刻调用 `extractNumbers()`。
3. 立刻调用 `analysis.observe(values)`。
4. 再把 `rules.observe(values)` 的结果写回串口。
5. 再把 `analysis.buildReportLines()` 或 `rules.buildReportLines()` 的结果交给界面层。

这样做的好处是：

- 分析逻辑和串口读取逻辑分离。
- UI 只负责显示，不负责计算。
- 以后如果换成文件输入或网络输入，也能复用同一套分析模块。

## 如何融入 UI

如果你要把新分析结果显示到界面里，推荐这样做：

1. 先在 [src/monitor_ui.hpp](src/monitor_ui.hpp) 的 `MonitorViewState` 里增加一个字段，比如 `analysisRuleLines`。
2. 在 [src/system.cpp](src/system.cpp) 里把 `AnalysisRules::buildReportLines()` 的结果放进去。
3. 在 [src/monitor_ui.cpp](src/monitor_ui.cpp) 的“数据分析”页布局中，新增一个 `drawPanel()` 区块。

例如：

```cpp
drawPanel(window, font, sf::FloatRect(264.f, 404.f, 752.f, 150.f), "触发结果", state.analysisRuleLines);
```

### 当前项目里对应的实际位置

你现在这个项目里，调用链已经提前接好了，所以如果你要继续扩展，只要关注这几个点：

- [src/system.cpp](src/system.cpp) 的 `updateStateFromLine()`：这里负责把一行串口数据拆成数字并喂给分析器。
- [src/system.cpp](src/system.cpp) 的 `buildMonitorViewState()`：这里负责把分析结果打包给 UI。
- [src/reading_analysis.cpp](src/reading_analysis.cpp) 的 `ReadingAnalysis::observe()`：这里是你以后写统计、报警、趋势逻辑的核心入口。
- [src/analysis_rules.cpp](src/analysis_rules.cpp) 的 `AnalysisRules::observe()`：这里是你以后写蜂鸣器、灌溉、开灯等规则控制逻辑的核心入口。
- [src/monitor_ui.cpp](src/monitor_ui.cpp) 的 `drawDataScreen()`：这里是把分析结果显示出来的位置。

如果你想把自己的分析模块单独拆成新类，也可以照这个流程接：

```cpp
const std::vector<double> values = extractNumbers(line);
myAnalysis.update(values);
const std::vector<std::string> lines = myAnalysis.buildReportLines();
```

如果你还需要把结果写回串口，可以再加一层：

```cpp
const std::vector<char> commands = myRules.observe(values);
for (char command : commands)
{
	reader.writeText(std::string(1, command), sendError);
}
```

然后把 `lines` 塞进 UI 的某个面板即可。

## 如果你想继续往下做

比较自然的下一步是做这三件事之一：

1. 把“数据分析”页从占位页改成真正的统计面板。
2. 在 `ReadingAnalysis` 里加入阈值报警和滑动平均。
3. 新增历史缓存，显示最近 N 条数据的趋势。

如果你愿意，我下一步可以直接帮你把“数据分析”页做成真正可用的第一页分析面板。