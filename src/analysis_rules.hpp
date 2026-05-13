#pragma once

#include <vector>
#include <string>

// 规则分析：根据传感器数据判断是否需要触发动作
class AnalysisRules
{
public:
    // 观察一行数据，返回需要发送给ESP32的指令字符列表
    std::vector<char> observe(const std::vector<double> &values);

    // 生成触发状态报告行
    std::vector<std::string> buildReportLines() const;
};
