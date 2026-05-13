#include "number_parser.hpp"
#include <sstream>
#include <cctype>

std::vector<double> extractNumbers(const std::string &line)
{
    std::vector<double> result;
    std::string current;

    for (char ch : line)
    {
        // 数字、小数点、负号属于数值的一部分
        if (std::isdigit(static_cast<unsigned char>(ch)) ||
            ch == '.' || ch == '-')
        {
            current += ch;
        }
        else if (!current.empty())
        {
            // 遇到分隔符，尝试转换已累积的数字
            try
            {
                result.push_back(std::stod(current));
            }
            catch (...)
            {
                // 跳过无法转换的内容
            }
            current.clear();
        }
    }

    // 处理行末最后一个数字
    if (!current.empty())
    {
        try
        {
            result.push_back(std::stod(current));
        }
        catch (...) {}
    }

    return result;
}
