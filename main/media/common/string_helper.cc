#include "string_helper.h"
std::string StringHelper::ReplaceString(const std::string& str, const std::string& from,
                                        const std::string& to) {
    std::string result = str;
    size_t start_pos = result.find(from);
    if (start_pos != std::string::npos) {
        result.replace(start_pos, from.length(), to);
    }
    return result;
}
std::string StringHelper::ReplaceStringAll(std::string& str, const std::string& from,
                                           const std::string& to) {
    std::string result = str;
    size_t start_pos = 0;
    while ((start_pos = result.find(from, start_pos)) != std::string::npos) {
        result.replace(start_pos, from.length(), to);
        start_pos += to.length();  // 避免无限循环，跳到下一个位置
    }
    return result;
}
std::string StringHelper::MillisecondToString(const long milliseconds) {
    long days = milliseconds / (24 * 60 * 60 * 1000);
    long hours = (milliseconds % (24 * 60 * 60 * 1000)) / (60 * 60 * 1000);
    long minutes = (milliseconds % (60 * 60 * 1000)) / (60 * 1000);
    long seconds = (milliseconds % (60 * 1000)) / 1000;
    std::string result;
    if (days > 0) {
        result += std::to_string(days) + ".";
    }
    if (hours > 0) {
        result += std::to_string(hours) + ":";
    } else if (!result.empty()) {
        result += "00:";
    }
    if (minutes > 0) {
        result += std::to_string(minutes) + ":";
    } else if (!result.empty()) {
        result += "00:";
    }
    result += std::to_string(seconds) + " s";
    return result;
}
