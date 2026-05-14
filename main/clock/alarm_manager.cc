
#include "alarm_manager.h"
#include <esp_log.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <time.h>
#include <cstring>
#include <mutex>
#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "device_state.h"
#include "display.h"
#include "mcp_server.h"
#include "media/restful_client.h"

static const char* TAG = "AlarmManager";
#define RECORD_FILE_NAME "alarm_record"

// 单例实例
AlarmManager& AlarmManager::GetInstance() {
    static AlarmManager instance;
    return instance;
}

AlarmManager::AlarmManager() : timer_handle_(nullptr), is_ringing_(false) {}

AlarmManager::~AlarmManager() {
    if (timer_handle_) {
        esp_timer_stop(timer_handle_);
        esp_timer_delete(timer_handle_);
    }
}

void AlarmManager::Initialize() {
    time_t now = time(nullptr);
    struct tm* tm = localtime(&now);
    ESP_LOGI(TAG, "Initializing AlarmManager at %s", asctime(tm));

    // 加载闹钟配置
    LoadAlarms();

    // 创建定时器
    esp_timer_create_args_t timer_args = {
        .callback = TimerCallback, .arg = this, .name = "alarm_timer"};
    esp_timer_create(&timer_args, &timer_handle_);

    // 更新定时器
    UpdateTimer();

    ESP_LOGI(TAG, "AlarmManager initialized with %d alarms", alarms_.size());
}

void AlarmManager::GenerateMcpServerTools(std::vector<McpTool*>& tools) {
    // std::string id;          // 闹钟唯一ID
    // std::string name;        // 闹钟名称
    // int hour;                // 小时 (0-23)
    // int minute;              // 分钟 (0-59)
    // int second;              // 秒 (0-59)
    // int volume;              // 音量 (0-100, 默认90)
    // RepeatMode repeat_mode;  // 重复模式
    // int repeat_days;         // 自定义重复日期（周日到周六，共7天,每个位表示一个天）

    auto tool = new McpTool(
        "self.alarm_clock.add",
        "This alarm clock is defined by a structured data model designed for precision and "
        "flexibility. It utilizes an Alarm struct that operates on absolute time, requiring any "
        "relative time settings to be converted into a specific hour, minute, and second format. "
        "Each alarm is uniquely identified by an ID and includes a customizable name and a volume "
        "level ranging from 0 to 100. The scheduling logic is handled by the RepeatMode enum, "
        "which supports single occurrences (ONCE), daily repeats (DAILY), workday-only schedules "
        "(WORKDAYS), holidays (HOLIDAYS), or fully customized patterns (CUSTOM). For custom "
        "setups, the repeat_daysinteger acts as a bitmask covering Sunday through Saturday, "
        "allowing users to select specific days of the week for the alarm to trigger.",
        PropertyList({Property("id", kPropertyTypeString), Property("name", kPropertyTypeString),
                      Property("hour", kPropertyTypeInteger, 0, 23),
                      Property("minute", kPropertyTypeInteger, 0, 59),
                      Property("second", kPropertyTypeInteger, 0, 59),
                      Property("volume", kPropertyTypeInteger, 90, 0, 100),
                      Property("repeat_mode", kPropertyTypeInteger, 0, 4),
                      Property("repeat_days", kPropertyTypeInteger, 0, 127)}),
        [this](const PropertyList& properties) -> ReturnValue {
            ESP_LOGI(TAG, "Add alarm");
            Alarm alarm(properties["id"].value<std::string>(),
                        properties["name"].value<std::string>(), properties["hour"].value<int>(),
                        properties["minute"].value<int>(), properties["second"].value<int>(),
                        properties["volume"].value<int>(),
                        (RepeatMode)properties["repeat_mode"].value<int>(),
                        properties["repeat_days"].value<int>());

            AddAlarm(alarm);
            return alarm.toJson();
        });
    tools.push_back(tool);
    tool = new McpTool(
        "self.alarm_clock.find",
        // 根据闹钟名称模糊查找闹钟,如果名称为空，返回所有闹钟，否则返回匹配的闹钟。结果为json数组
        "Fuzzily search for alarms based on the alarm name. If the name is empty, return all "
        "alarms; otherwise, return the matching alarms. The result is a JSON array.",
        PropertyList({Property("name", kPropertyTypeString)}),
        [this](const PropertyList& properties) -> ReturnValue {
            std::string name = properties["name"].value<std::string>();
            std::vector<Alarm> alarms;
            if (Alarm::findByName(alarms_, name, alarms)) {
                return Alarm::toJsonArray(alarms);
            }
            return "";
        });
    tools.push_back(tool);
    tool = new McpTool(
        "self.alarm_clock.delete",
        // 根据闹钟名称模糊删除闹钟,如果名称为空，删除所有闹钟，否则删除匹配的闹钟。
        "Fuzzily delete alarms based on the alarm name. If the name is empty, delete all "
        "alarms; otherwise, delete the matching alarms.",
        PropertyList({Property("name", kPropertyTypeString)}),
        [this](const PropertyList& properties) -> ReturnValue {
            std::string name = properties["name"].value<std::string>();
            std::vector<Alarm> alarms;
            if (Alarm::findByName(alarms_, name, alarms)) {
                for (const auto& alarm : alarms) {
                    RemoveAlarm(alarm.id);
                }
                return Alarm::toJsonArray(alarms);
            }
            return "";         
        });
    tools.push_back(tool);
}
void AlarmManager::LoadAlarms() {
    alarms_.clear();
    // alarms_.push_back(Alarm("default", "default", 0, 0, 10, 30));
    // alarms_.push_back(Alarm("default2", "default2", 0, 0, 30, 30));
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(RECORD_FILE_NAME, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS %s, using default alarms", RECORD_FILE_NAME);
        return;
    }

    // 读取闹钟数量
    int32_t alarm_count = 0;
    err = nvs_get_i32(nvs_handle, "count", &alarm_count);
    if (err != ESP_OK || alarm_count <= 0) {
        ESP_LOGW(TAG, "No alarms found in NVS");
        nvs_close(nvs_handle);
        return;
    }

    // 读取每个闹钟
    for (int i = 0; i < alarm_count; i++) {
        char key[32];
        char value[512];
        size_t length = sizeof(value);

        snprintf(key, sizeof(key), "alarm_%d", i);
        err = nvs_get_str(nvs_handle, key, value, &length);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to read alarm %d", i);
            continue;
        }

        // 解析闹钟配置
        Alarm alarm;
        auto json = std::string(value);
        if (!alarm.fromJson(json)) {
            continue;
        }
        alarms_.push_back(alarm);
    }

    nvs_close(nvs_handle);
}

void AlarmManager::SaveAlarms() {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(RECORD_FILE_NAME, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS %s for writing", RECORD_FILE_NAME);
        return;
    }

    // 清除旧数据
    err = nvs_erase_all(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase NVS");
        nvs_close(nvs_handle);
        return;
    }

    // 相对时间闹钟和一次性闹钟不保存
    auto save_alarms = std::vector<Alarm>();
    for (const auto& alarm : alarms_) {
        if (alarm.repeat_mode == RepeatMode::ONCE) {
            continue;
        }
        save_alarms.push_back(alarm);
    }
    ESP_LOGI(TAG, "Saving %d alarms to NVS %s", save_alarms.size(), RECORD_FILE_NAME);
    // 保存闹钟数量
    err = nvs_set_i32(nvs_handle, "count", save_alarms.size());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save alarm count");
        nvs_close(nvs_handle);
        return;
    }

    // 保存每个闹钟
    for (size_t i = 0; i < save_alarms.size(); i++) {
        const Alarm& alarm = save_alarms[i];
        char key[32];
        auto json = alarm.toJson();
        auto value = json.c_str();

        snprintf(key, sizeof(key), "alarm_%d", (int)i);
        err = nvs_set_str(nvs_handle, key, value);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save alarm %d", (int)i);
        }
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS");
    }

    nvs_close(nvs_handle);
}

void AlarmManager::LoadHolidays() {
    // 添加中国节假日（可根据需要扩展）
    holidays_.clear();

    // 获取当前年份
    time_t now = time(nullptr);
    struct tm* tm = localtime(&now);
    int current_year = tm->tm_year + 1900;

    // 网络获取节假日配置
    std::string url = "https://api.jiejiariapi.com/v1/holidays/" + std::to_string(current_year);
    RestfulClient restful_client;
    std::string response = restful_client.Get(url);
    if (response.empty()) {
        ESP_LOGW(TAG, "Failed to fetch holidays from network");
        return;
    }

    // 使用 cJSON 解析响应
    cJSON* root = cJSON_Parse(response.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse holiday JSON");
        return;
    }

    // 这个json不是数组，是一个对象
    //  {"2026-01-01":{"date":"2026-01-01","name":"元旦","isOffDay":true},"2026-01-02":{"date":"2026-01-02","name":"元旦","isOffDay":true}}
    cJSON* current_item = root->child;
    while (current_item != NULL) {
        // current_item->string 就是 "2026-01-01" 这个 Key

        cJSON* date_obj = cJSON_GetObjectItem(current_item, "date");
        cJSON* name_obj = cJSON_GetObjectItem(current_item, "name");
        cJSON* is_off_day_obj = cJSON_GetObjectItem(current_item, "isOffDay");

        if (date_obj && name_obj && is_off_day_obj) {
            const char* date_str = cJSON_GetStringValue(date_obj);
            const char* name = cJSON_GetStringValue(name_obj);
            bool is_off_day = cJSON_IsTrue(is_off_day_obj);

            // 解析日期格式: YYYY-MM-DD
            int year, month, day;
            if (sscanf(date_str, "%d-%d-%d", &year, &month, &day) == 3) {
                holidays_.emplace_back(month, day, name, is_off_day);
                ESP_LOGI(TAG, "Loaded holiday: %s (%02d-%02d), is_off_day: %d", name, month, day,
                         is_off_day);
            }
        }

        // 6. 移动到下一个兄弟节点（下一个日期）
        current_item = current_item->next;
    }

    cJSON_Delete(root);

    // 如果没有加载到节假日，使用内置数据
    if (holidays_.empty()) {
        ESP_LOGW(TAG, "No holidays loaded from network, using built-in data");
    }
}

bool AlarmManager::AddAlarm(const Alarm& alarm) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 检查ID是否已存在
    for (const auto& a : alarms_) {
        if (a.id == alarm.id) {
            ESP_LOGW(TAG, "Alarm ID already exists: %s", alarm.id.c_str());
            return false;
        }
    }

    alarms_.push_back(alarm);
    SaveAlarms();
    UpdateTimerLocked();

    ESP_LOGI(TAG, "Added alarm: %s (%02d:%02d:%02d)", alarm.name.c_str(), alarm.hour, alarm.minute,
             alarm.second);
    return true;
}

bool AlarmManager::RemoveAlarm(const std::string& alarm_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return RemoveAlarmLocked(alarm_id);
}
bool AlarmManager::RemoveAlarmLocked(const std::string& alarm_id) {
    auto it = std::find_if(alarms_.begin(), alarms_.end(),
                           [&](const Alarm& a) { return a.id == alarm_id; });

    if (it == alarms_.end()) {
        ESP_LOGW(TAG, "Alarm not found: %s", alarm_id.c_str());
        return false;
    }

    alarms_.erase(it);
    SaveAlarms();
    UpdateTimerLocked();

    ESP_LOGI(TAG, "Removed alarm: %s", alarm_id.c_str());
    return true;
}

bool AlarmManager::RemoveAllAlarms() {
    std::lock_guard<std::mutex> lock(mutex_);
    alarms_.clear();
    SaveAlarms();
    UpdateTimerLocked();
    return true;
}

std::vector<Alarm> AlarmManager::GetAlarms() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return alarms_;
}

bool AlarmManager::GetAlarm(const std::string& alarm_id, Alarm& out_alarm) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = std::find_if(alarms_.begin(), alarms_.end(),
                           [&](const Alarm& a) { return a.id == alarm_id; });

    if (it == alarms_.end()) {
        return false;
    }

    out_alarm = *it;
    return true;
}

bool AlarmManager::UpdateAlarm(const Alarm& alarm) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = std::find_if(alarms_.begin(), alarms_.end(),
                           [&](const Alarm& a) { return a.id == alarm.id; });

    if (it == alarms_.end()) {
        ESP_LOGW(TAG, "Alarm not found: %s", alarm.id.c_str());
        return false;
    }

    *it = alarm;
    SaveAlarms();
    UpdateTimerLocked();

    ESP_LOGI(TAG, "Updated alarm: %s (%02d:%02d:%02d)", alarm.name.c_str(), alarm.hour,
             alarm.minute, alarm.second);
    return true;
}

bool AlarmManager::SetAlarmEnabled(const std::string& alarm_id, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = std::find_if(alarms_.begin(), alarms_.end(),
                           [&](const Alarm& a) { return a.id == alarm_id; });

    if (it == alarms_.end()) {
        ESP_LOGW(TAG, "Alarm not found: %s", alarm_id.c_str());
        return false;
    }

    it->state = enabled ? AlarmState::ENABLED : AlarmState::DISABLED;
    SaveAlarms();
    UpdateTimerLocked();

    ESP_LOGI(TAG, "Alarm %s %s", alarm_id.c_str(), enabled ? "enabled" : "disabled");
    return true;
}

void AlarmManager::StopRinging() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (is_ringing_) {
        is_ringing_ = false;

        CallAlarmStopCallback(current_ringing_alarm_);

        // 更新闹钟状态
        auto it = std::find_if(alarms_.begin(), alarms_.end(),
                               [&](const Alarm& a) { return a.id == current_ringing_alarm_.id; });
        if (it != alarms_.end()) {
            it->state = AlarmState::ENABLED;
            if (it->repeat_mode == RepeatMode::ONCE) {
                RemoveAlarmLocked(it->id);
            }
        }

        UpdateTimerLocked();
        ESP_LOGI(TAG, "Alarm stopped");
    }
}

void AlarmManager::Snooze() {
    if (original_volume_ > 0) {
        auto& board = Board::GetInstance();
        auto audio_codec = board.GetAudioCodec();
        audio_codec->SetOutputVolume(original_volume_);
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (!is_ringing_) {
        ESP_LOGW(TAG, "No alarm is ringing");
        return;
    }

    // 检查贪睡次数
    if (current_ringing_alarm_.snooze_count == 0) {
        ESP_LOGW(TAG, "Snooze count exceeded");
        StopRinging();
        return;
    }

    // 减少贪睡次数
    if (current_ringing_alarm_.snooze_count > 0) {
        current_ringing_alarm_.snooze_count--;
    }

    // 停止当前响铃
    is_ringing_ = false;

    CallAlarmStopCallback(current_ringing_alarm_);

    // 设置贪睡定时器
    int64_t snooze_ms = (int64_t)current_ringing_alarm_.snooze_duration * 60 * 1000;
    esp_timer_stop(timer_handle_);
    esp_timer_start_once(timer_handle_, snooze_ms);

    ESP_LOGI(TAG, "Alarm snoozed for %d minutes", current_ringing_alarm_.snooze_duration);
}

void AlarmManager::SetAlarmCallback(AlarmCallback callback) { alarm_callback_ = callback; }

void AlarmManager::SetAlarmStopCallback(AlarmStopCallback callback) {
    alarm_stop_callback_ = callback;
}

void AlarmManager::AddHoliday(const Holiday& holiday) {
    std::lock_guard<std::mutex> lock(mutex_);
    holidays_.push_back(holiday);
}

std::vector<Holiday> AlarmManager::GetHolidays() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return holidays_;
}

bool AlarmManager::IsHoliday(int month, int day, int weekday) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (holidays_.empty()) {
        return weekday == 0 || weekday == 6;
    }
    for (const auto& holiday : holidays_) {
        if (holiday.month == month && holiday.day == day && holiday.is_offday) {
            return true;
        }
    }
    return false;
}

bool AlarmManager::IsWorkday(int month, int day, int weekday) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (holidays_.empty()) {
        return weekday != 0 && weekday != 6;
    }

    for (const auto& holiday : holidays_) {
        if (holiday.month == month && holiday.day == day && holiday.is_offday) {
            return false;
        }
    }
    return true;
}

bool AlarmManager::ShouldRingToday(const Alarm& alarm) const {
    time_t now = time(nullptr);
    struct tm* tm = localtime(&now);
    int weekday = tm->tm_wday;   // 0=周日, 1=周一, ..., 6=周六
    int month = tm->tm_mon + 1;  // 1-12
    int day = tm->tm_mday;       // 1-31
    ESP_LOGI(TAG,
             "ShouldRingToday Current time: %02d-%02d-%02d, weekday: %d, repeat mode: %d , repeat "
             "days: %d",
             alarm.hour, alarm.minute, alarm.second, weekday, alarm.repeat_mode, alarm.repeat_days);
    switch (alarm.repeat_mode) {
        case RepeatMode::ONCE: {
            // 检查是否是设置闹钟的当天（简化处理）
            // 实际应该存储闹钟设置的日期
            return true;
        }
        case RepeatMode::DAILY:
            return true;
        case RepeatMode::WORKDAYS:
            return IsWorkday(month, day, weekday);
        case RepeatMode::HOLIDAYS:
            return IsHoliday(month, day, weekday);
        case RepeatMode::CUSTOM:
            return (alarm.repeat_days >> weekday & (1 << weekday)) != 0;
        default:
            return false;
    }
}
char* to_string(int64_t val) {
    static char buf[20];
    itoa(val, buf, 10);
    return buf;
}
int64_t AlarmManager::CalculateNextRingTime(const Alarm& alarm) const {
    time_t now = time(nullptr);
    auto alarm_time = alarm.toTime(now);
    // 计算延迟时间（微秒）
    auto ret = (int64_t)(difftime(alarm_time, now) * 1000000);
    return ret;
}

void AlarmManager::UpdateTimer() {
    std::lock_guard<std::mutex> lock(mutex_);
    UpdateTimerLocked();
}

void AlarmManager::UpdateTimerLocked() {
    esp_timer_stop(timer_handle_);

    int64_t min_delay = INT64_MAX;
    Alarm next_alarm;

    // 找到最早响铃的闹钟
    for (const auto& alarm : alarms_) {
        if (alarm.state != AlarmState::ENABLED) {
            continue;
        }

        // 检查今天是否应该响铃
        if (!ShouldRingToday(alarm)) {
            continue;
        }

        int64_t delay = CalculateNextRingTime(alarm);
        if (delay > 0 && delay < min_delay) {
            min_delay = delay;
            next_alarm = alarm;
        }
    }

    // 如果有闹钟需要响铃，设置定时器
    if (min_delay != INT64_MAX) {
        esp_timer_start_once(timer_handle_, min_delay);
        ESP_LOGI(TAG, "Next alarm: %s at %02d:%02d:%02d (in %s ms)", next_alarm.name.c_str(),
                 next_alarm.hour, next_alarm.minute, next_alarm.second,
                 to_string(min_delay / 1000));
    }
}

void AlarmManager::TimerCallback(void* arg) {
    AlarmManager* manager = static_cast<AlarmManager*>(arg);
    manager->OnAlarmTriggered();
}

void AlarmManager::OnAlarmTriggered() {
    std::lock_guard<std::mutex> lock(mutex_);

    time_t now = time(nullptr);
    struct tm* tm = localtime(&now);

    // 找到应该响铃的闹钟
    for (auto& alarm : alarms_) {
        if (alarm.state != AlarmState::ENABLED) {
            continue;
        }

        // 检查时间是否匹配（允许1秒误差）
        if (abs(alarm.hour - tm->tm_hour) > 0 || abs(alarm.minute - tm->tm_min) > 0 ||
            abs(alarm.second - tm->tm_sec) > 1) {
            continue;
        }

        // 检查是否应该今天响铃
        if (!ShouldRingToday(alarm)) {
            continue;
        }

        // 设置为响铃状态
        alarm.state = AlarmState::RINGING;
        alarm.start_ring_time = now;
        current_ringing_alarm_ = alarm;
        is_ringing_ = true;

        CallAlarmCallback(alarm);

        ESP_LOGI(TAG, "Alarm triggered: %s (%02d:%02d:%02d)", alarm.name.c_str(), alarm.hour,
                 alarm.minute, alarm.second);
        // 更新定时器（设置下一个闹钟）
        UpdateTimerLocked();
    }

    // 如果没有找到匹配的闹钟，更新定时器
    UpdateTimerLocked();
    ESP_LOGI(TAG, "No alarm triggered");
}
void AlarmManager::CallAlarmCallback(const Alarm& alarm) {
    // 保存当前系统音量
    auto& board = Board::GetInstance();
    auto audio_codec = board.GetAudioCodec();
    original_volume_ = audio_codec->output_volume();
    // audio_codec->SetOutputVolume(alarm.volume);

    auto& app = Application::GetInstance();
    auto display = Board::GetInstance().GetDisplay();
    display->SetChatMessage("system", alarm.name.c_str());

    // 触发回调
    if (alarm_callback_) {
        alarm_callback_(alarm);
    }
    app.AppendEventToGroup(MAIN_EVENT_ARARM_CLOCK_RINGING);
}

void AlarmManager::CallAlarmStopCallback(const Alarm& alarm) {
    if (original_volume_ > 0) {
        auto& board = Board::GetInstance();
        auto audio_codec = board.GetAudioCodec();
        audio_codec->SetOutputVolume(original_volume_);
    }

    auto& app = Application::GetInstance();

    if (alarm_stop_callback_) {
        alarm_stop_callback_(alarm);
    }
    app.ClearEventFromGroup(MAIN_EVENT_ARARM_CLOCK_RINGING);
}

bool AlarmManager::IsRinging() const { return is_ringing_; }

void AlarmManager::GetCurrentRingingAlarm(Alarm& out_alarm) const {
    out_alarm = current_ringing_alarm_;
}