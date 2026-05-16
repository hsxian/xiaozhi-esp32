#include "alarm.h"
#include <esp_log.h>
#include <cJSON.h>

#define TAG "Alarm"

// 默认构造函数
Alarm::Alarm()
    : hour(0),
      minute(0),
      second(0),
      volume(80),
      repeat_mode(RepeatMode::ONCE),
      repeat_days(0),
      state(AlarmState::ENABLED),
      snooze_duration(5),
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
          snooze_duration(5),
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
    cJSON_AddNumberToObject(root, "repeat_days", repeat_days);
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
    repeat_days = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "repeat_days"));
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
    int64_t now_s = (int64_t)now ;
    int64_t alarm_s =(int64_t)alarm_time ;

    // 如果今天的闹钟时间已经过了，计算明天的
    if (alarm_s <= now_s) {
        alarm_s += 24 * 60 * 60;
    }
    return alarm_s;
}

std::string Alarm::ToJsonArray(std::vector<Alarm>& alarms) {
    cJSON* array = cJSON_CreateArray();
    for (const auto& alarm : alarms) {
        cJSON* item = cJSON_CreateObject();
        alarm.ToJson(item);
        cJSON_AddItemToArray(array, item);
    }
    auto json_str = cJSON_Print(array);
    cJSON_Delete(array);
    return json_str;
}

bool Alarm::findByName(std::vector<Alarm>& alarms, const std::string& name,
                       std::vector<Alarm>& found_alarms) {
    found_alarms.clear();
    for (const auto& alarm : alarms) {
        if (alarm.name.find(name) != std::string::npos) {
            found_alarms.push_back(alarm);
        }
    }
    return !found_alarms.empty();
}