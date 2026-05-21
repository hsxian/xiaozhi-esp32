#pragma once

#include <string>

class StringHelper {
public:
    std::string ReplaceString(const std::string& str, const std::string& from,
                                const std::string& to);
    std::string ReplaceStringAll(std::string& str, const std::string& from,
                                 const std::string& to);
    // 毫秒转换为字符串（格式Day.HH:MM:SS）
    std::string MillisecondToString(const long milliseconds);
};