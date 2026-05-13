#include "analysis_rules.hpp"

//=====================================================================//
//                    传感器阈值（可根据实际环境调整）                     //
//=====================================================================//
#define TEMP_MIN     0.0     // 温度下限 (°C)
#define TEMP_MAX     45.0    // 温度上限 (°C)
#define HUMI_MIN     10.0    // 湿度下限 (%)
#define HUMI_MAX     90.0    // 湿度上限 (%)
#define SOIL_MIN     10.0    // 土壤湿度下限 — 低于此值需要浇水 (%)
#define SOIL_MAX     90.0    // 土壤湿度上限 (%)
#define LIGHT_MIN    5.0     // 光照下限
#define LIGHT_MAX    95.0    // 光照上限
//=====================================================================//

std::vector<char> AnalysisRules::observe(const std::vector<double> &values)
{
    std::vector<char> cmds;
    if (values.size() < 4)
        return cmds;

    double temp  = values[0];
    double humi  = values[1];
    double soil  = values[2];
    double light = values[3];

    if (light < LIGHT_MIN || light > LIGHT_MAX) cmds.push_back('1');
    if (humi  < HUMI_MIN  || humi  > HUMI_MAX ) cmds.push_back('2');
    if (temp  < TEMP_MIN  || temp  > TEMP_MAX ) cmds.push_back('3');
    if (soil  < SOIL_MIN  || soil  > SOIL_MAX ) cmds.push_back('4');

    return cmds;
}

std::vector<std::string> AnalysisRules::buildReportLines() const
{
    return {}; // 暂不扩展
}
