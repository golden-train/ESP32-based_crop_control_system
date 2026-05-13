#pragma once

#include <vector>
#include <string>

// 从一行文本中提取所有数字（支持整数和小数）
// 例如 "25.5, 60.2, 45.0, 850" → {25.5, 60.2, 45.0, 850.0}
std::vector<double> extractNumbers(const std::string &line);
