#include "alarm.h"
#include <esp_log.h>
#include <cJSON.h>
#include <cstring>
#include <format>
#include <sstream>

#define TAG "Alarm"

// repeat_days 位掩码 → 字符串 "0,1,2,3,4,5,6"
std::string RepeatDaysToString(uint8_t repeat_days) {
    if (repeat_days == 0)
        return "";
    std::string result;
    for (int i = 0; i < 7; i++) {
        if (repeat_days & (1 << i)) {
            if (!result.empty())
                result += ",";
            result += std::to_string(i);
        }
    }
    return result;
}

// 字符串 "0,1,2,3,4,5,6" → repeat_days 位掩码
uint8_t StringToRepeatDays(const std::string& str) {
    if (str.empty())
        return 0;
    uint8_t result = 0;
    std::stringstream ss(str);
    std::string token;
    while (std::getline(ss, token, ',')) {
        int day = std::stoi(token);
        if (day >= 0 && day <= 6) {
            result |= (1 << day);
        }
    }
    return result;
}

// 默认构造函数
Alarm::Alarm()
    : hour(0),
      minute(0),
      second(0),
      volume(80),
      repeat_mode(RepeatMode::ONCE),
      repeat_days(0),
      state(AlarmState::ENABLED),
      snooze_duration(2),
      snooze_count(6) {}

// 带参数构造函数
Alarm::Alarm(const std::string& alarm_id, const std::string& alarm_name, int h, int m, int s,
             int vol, RepeatMode mode, int repeat_days)
    : id(alarm_id),
      name(alarm_name),
      hour(h),
      minute(m),
      second(s),
      volume(vol),
      repeat_mode(mode),
      repeat_days(repeat_days),
      state(AlarmState::ENABLED),
      snooze_duration(2),
      snooze_count(6) {
    ESP_LOGI(TAG,
             "Alarm created: %s(%s), %02d:%02d:%02d, volume: %d, repeat mode: %d, repeat days: %d",
             name.c_str(), id.c_str(), hour, minute, second, volume, repeat_mode, repeat_days);
}

// 转换为JSON字符串
std::string Alarm::ToJson() const {
    cJSON* root = cJSON_CreateObject();
    ToJson(root);
    auto json_str = cJSON_Print(root);
    cJSON_Delete(root);
    return json_str;
}

void Alarm::ToJson(cJSON* root) const {
    cJSON_AddStringToObject(root, "id", id.c_str());
    cJSON_AddStringToObject(root, "name", name.c_str());
    cJSON_AddNumberToObject(root, "hour", hour);
    cJSON_AddNumberToObject(root, "minute", minute);
    cJSON_AddNumberToObject(root, "second", second);
    cJSON_AddNumberToObject(root, "volume", volume);
    cJSON_AddNumberToObject(root, "repeat", (int)repeat_mode);
    cJSON_AddStringToObject(root, "repeat_days", RepeatDaysToString(repeat_days).c_str());
    cJSON_AddNumberToObject(root, "state", (int)state);
    cJSON_AddNumberToObject(root, "snooze_duration", snooze_duration);
    cJSON_AddNumberToObject(root, "snooze_count", snooze_count);
}

// 从JSON字符串解析
bool Alarm::FromJson(const std::string& json) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == NULL) {
        ESP_LOGE(__func__, "Failed to parse JSON: %s", cJSON_GetErrorPtr());
        return false;
    }

    cJSON* idJson = cJSON_GetObjectItem(root, "id");
    if (idJson == NULL) {
        ESP_LOGE(__func__, "ID not found in JSON");
        cJSON_Delete(root);
        return false;
    }
    id = std::string(cJSON_GetStringValue(idJson));
    name = std::string(cJSON_GetStringValue(cJSON_GetObjectItem(root, "name")));
    hour = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "hour"));
    minute = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "minute"));
    second = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "second"));
    volume = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "volume"));
    repeat_mode = (RepeatMode)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "repeat"));
    cJSON* repeatDaysJson = cJSON_GetObjectItem(root, "repeat_days");
    if (cJSON_IsString(repeatDaysJson)) {
        repeat_days = StringToRepeatDays(cJSON_GetStringValue(repeatDaysJson));
    } else if (cJSON_IsNumber(repeatDaysJson)) {
        repeat_days = (uint8_t)cJSON_GetNumberValue(repeatDaysJson);
    } else {
        repeat_days = 0;
    }
    state = (AlarmState)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "state"));
    snooze_duration = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "snooze_duration"));
    snooze_count = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "snooze_count"));

    cJSON_Delete(root);
    return true;
}

time_t Alarm::toTime(const time_t& now) const {
    struct tm* tm = localtime(&now);
    // 计算今天的闹钟时间
    struct tm alarm_tm = *tm;

    alarm_tm.tm_hour = hour;
    alarm_tm.tm_min = minute;
    alarm_tm.tm_sec = second;

    time_t alarm_time = mktime(&alarm_tm);
    return alarm_time;
}

std::string Alarm::ToJsonArray(std::vector<Alarm*>& alarms) {
    cJSON* array = cJSON_CreateArray();
    for (const auto& alarm : alarms) {
        cJSON* item = cJSON_CreateObject();
        alarm->ToJson(item);
        cJSON_AddItemToArray(array, item);
    }
    auto json_str = cJSON_Print(array);
    cJSON_Delete(array);
    return json_str;
}

std::vector<Alarm*> Alarm::findByName(std::vector<Alarm*>& alarms, const std::string& name) {
    std::vector<Alarm*> found_alarms;
    for (const auto& alarm : alarms) {
        if (alarm->name.find(name) != std::string::npos) {
            found_alarms.push_back(alarm);
        }
    }
    return found_alarms;
}

std::string Alarm::ToString() const {
    std::string week = "";
    if (repeat_mode == RepeatMode::CUSTOM) {
        if (repeat_days == REPEAT_DAYS_EVERYDAY) {
            week = "(Every day)";
        } else if (repeat_days == REPEAT_DAYS_WORKDAYS) {
            week = "(Workdays)";
        } else if (repeat_days == REPEAT_DAYS_WEEKENDS) {
            week = "(Weekends)";
        } else if (repeat_days == REPEAT_DAYS_NONE) {
            week = "(repeat days value error)";
        } else {
            static const char* week_names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
            for (int i = 0; i < 7; i++) {
                if (repeat_days & (1 << i)) {
                    week += week_names[i];
                    week += " ";
                }
            }
            week = "(" + week + ")";
        }
    }

    std::string ret = std::format(
        "Alarm(id={}, name={}, time={:02d}:{:02d}:{:02d}, volume={}, repeat_mode={}, "
        "repeat_days={}{}, snooze_duration={}min, snooze_count={})",
        id, name, hour, minute, second, volume, (int)repeat_mode, repeat_days, week,
        snooze_duration, snooze_count);

    return ret;
}