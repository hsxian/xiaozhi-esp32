
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
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "device_state.h"
#include "display.h"
#include "mcp_server.h"
#include "media/common/restful_client.h"
#include "media/common/string_helper.h"
#include "media/common/xiaozhi_helper.h"

static const char* TAG = "AlarmManager";
#define RECORD_FILE_NAME "alarm_record"

// 单例实例
AlarmManager& AlarmManager::GetInstance() {
    static AlarmManager instance;
    return instance;
}

AlarmManager::AlarmManager()
    : timer_handle_(nullptr), is_ringing_(false), display_(Board::GetInstance().GetDisplay()) {
    Application::GetInstance().BeforeHandleWakeWordEventListener().AddEventListener(
        [this](void* data) { return this->OnWakeWordDetected(data); });
}

AlarmManager::~AlarmManager() {
    if (timer_handle_) {
        esp_timer_stop(timer_handle_);
        esp_timer_delete(timer_handle_);
    }
    for (auto& a : alarms_) {
        delete a;
    }
    alarms_.clear();
    current_ringing_alarm_ = nullptr;
}

void AlarmManager::Initialize() {
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    ESP_LOGI(TAG, "Initializing AlarmManager at %s", asctime(&tm));

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
        "setups, the repeat_days integer acts as a bitmask covering "
        "Sunday(1)、Monday(2)、Tuesday(4)、Wednesday(8)、Thursday(16)、Friday(32) and "
        "Saturday(64), "
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
            return alarm.ToJson();
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
            std::vector<Alarm*> alarms_found = Alarm::findByName(alarms_, name);
            return Alarm::ToJsonArray(alarms_found);
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
            std::vector<Alarm*> alarms_found = Alarm::findByName(alarms_, name);
            for (const auto& alarm : alarms_found) {
                RemoveAlarm(alarm->id);
            }
            return Alarm::ToJsonArray(alarms_found);
        });
    tools.push_back(tool);
}
void AlarmManager::LoadAlarms() {
    alarms_.clear();
    // alarms_.push_back(new Alarm("default", "default", 0, 0, 10, 30));
    // alarms_.push_back(new Alarm("default2", "default2", 0, 0, 30, 30));
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
        if (!alarm.FromJson(json)) {
            continue;
        }
        alarms_.push_back(new Alarm(alarm));
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
    auto save_alarms = std::vector<Alarm*>();
    for (const auto& alarm : alarms_) {
        if (alarm->repeat_mode == RepeatMode::ONCE) {
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
        const Alarm* alarm = save_alarms[i];
        char key[32];
        auto json = alarm->ToJson();
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

    ESP_LOGI(TAG, "Saved %d alarms to NVS %s", save_alarms.size(), RECORD_FILE_NAME);
}

void AlarmManager::LoadHolidays() {
    // 添加中国节假日（可根据需要扩展）
    holidays_.clear();

    // 获取当前年份
    time_t now = time(nullptr) + 2*60;
    struct tm tm;
    localtime_r(&now, &tm);
    int current_year = tm.tm_year + 1900;

    // 网络获取节假日配置
    std::string url =
        "https://api.jiejiariapi.com/v1/holidays/" + std::to_string(std::max(current_year, 2008));
    RestfulClient restful_client;
    std::string response = restful_client.Get(url);
    if (response.empty()) {
        ESP_LOGW(TAG, "Failed to fetch holidays from network");
        UpdateTimerLocked();
        return;
    }

    // 使用 cJSON 解析响应
    cJSON* root = cJSON_Parse(response.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse holiday JSON");
        UpdateTimerLocked();
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
    } else {
        UpdateTimerLocked();
    }
}

bool AlarmManager::AddAlarm(const Alarm& alarm) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 检查ID是否已存在
    for (const auto& a : alarms_) {
        if (a->id == alarm.id) {
            ESP_LOGW(TAG, "Alarm ID already exists: %s", alarm.id.c_str());
            return false;
        }
    }

    alarms_.push_back(new Alarm(alarm));
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
                           [&](const Alarm* a) { return a->id == alarm_id; });

    if (it == alarms_.end()) {
        ESP_LOGW(TAG, "Alarm not found: %s", alarm_id.c_str());
        return false;
    }

    ESP_LOGI(TAG, "Removed alarm: %s (%s)", (*it)->name.c_str(), alarm_id.c_str());
    if (*it == current_ringing_alarm_) {
        current_ringing_alarm_ = nullptr;
    }
    delete *it;
    alarms_.erase(it);
    SaveAlarms();
    UpdateTimerLocked();
    return true;
}

bool AlarmManager::RemoveAllAlarms() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& a : alarms_) {
        delete a;
    }
    alarms_.clear();
    SaveAlarms();
    UpdateTimerLocked();
    return true;
}

std::vector<Alarm*> AlarmManager::GetAlarms() const { return alarms_; }

Alarm* AlarmManager::GetAlarm(const std::string& alarm_id) const {
    auto it = std::find_if(alarms_.begin(), alarms_.end(),
                           [&](const Alarm* a) { return a->id == alarm_id; });

    if (it == alarms_.end()) {
        return nullptr;
    }

    return *it;
}

bool AlarmManager::UpdateAlarm(const Alarm& alarm) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto* existing = GetAlarm(alarm.id);
    if (!existing) {
        ESP_LOGW(TAG, "Alarm not found: %s", alarm.id.c_str());
        return false;
    }

    *existing = alarm;
    SaveAlarms();
    UpdateTimerLocked();

    ESP_LOGI(TAG, "Updated alarm: %s (%02d:%02d:%02d)", alarm.name.c_str(), alarm.hour,
             alarm.minute, alarm.second);
    return true;
}

bool AlarmManager::SetAlarmEnabled(const std::string& alarm_id, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto* alarm = GetAlarm(alarm_id);
    if (!alarm) {
        ESP_LOGW(TAG, "Alarm not found: %s", alarm_id.c_str());
        return false;
    }

    alarm->state = enabled ? AlarmState::ENABLED : AlarmState::DISABLED;
    SaveAlarms();
    UpdateTimerLocked();

    ESP_LOGI(TAG, "Alarm %s %s", alarm_id.c_str(), enabled ? "enabled" : "disabled");
    return true;
}

void AlarmManager::StopRinging() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (is_ringing_ && current_ringing_alarm_) {
        is_ringing_ = false;

        StopAlarmRinging(*current_ringing_alarm_);

        // 更新闹钟状态
        current_ringing_alarm_->state = AlarmState::ENABLED;
        if (current_ringing_alarm_->repeat_mode == RepeatMode::ONCE) {
            RemoveAlarmLocked(current_ringing_alarm_->id);
        }

        current_ringing_alarm_ = nullptr;
        UpdateTimerLocked();
        auto& app = Application::GetInstance();
        ESP_LOGI(TAG, "Alarm stopped");
    }
}

void AlarmManager::Snooze() {
    if (original_volume_ > 0) {
        auto& board = Board::GetInstance();
        auto audio_codec = board.GetAudioCodec();
        audio_codec->SetOutputVolume(original_volume_);
    }


    if (!is_ringing_ || !current_ringing_alarm_) {
        ESP_LOGW(TAG, "No alarm is ringing");
        return;
    }

    // 检查贪睡次数
    if (current_ringing_alarm_->snooze_count == 0) {
        ESP_LOGW(TAG, "Snooze count exceeded");
        StopRinging();
        return;
    }

    // 减少贪睡次数
    if (current_ringing_alarm_->snooze_count > 0) {
        current_ringing_alarm_->snooze_count--;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    // 停止当前响铃
    is_ringing_ = false;

    StopAlarmRinging(*current_ringing_alarm_);

    current_ringing_alarm_->state = AlarmState::SNOOZED;

    // 设置贪睡定时器
    int64_t snooze_us = (int64_t)current_ringing_alarm_->snooze_duration * 60 * 1000000;
    esp_timer_stop(timer_handle_);
    esp_timer_start_once(timer_handle_, snooze_us);

    auto msg = std::format("Alarm snoozed for {} minutes", current_ringing_alarm_->snooze_duration);
    auto msg_str = msg.c_str();
    ESP_LOGI(TAG, "%s", msg_str);
    display_->SetChatMessage("alarm", msg_str);
}

void AlarmManager::AddHoliday(const Holiday& holiday) { holidays_.push_back(holiday); }

std::vector<Holiday> AlarmManager::GetHolidays() const { return holidays_; }

bool AlarmManager::IsHoliday(int month, int day, int weekday) const {
    // 先查节假日列表中的特殊日期
    for (const auto& holiday : holidays_) {
        if (holiday.month == month && holiday.day == day) {
            return holiday.is_offday;  // true=节假日休息, false=调休上班
        }
    }
    // 不在特殊日期列表中，回退到周末判断
    return weekday == 0 || weekday == 6;
}

bool AlarmManager::IsWorkday(int month, int day, int weekday) const {
    // 先查节假日列表中的特殊日期
    for (const auto& holiday : holidays_) {
        if (holiday.month == month && holiday.day == day) {
            return !holiday.is_offday;  // true=调休上班, false=节假日休息
        }
    }
    // 不在特殊日期列表中，回退到工作日判断
    return weekday != 0 && weekday != 6;
}

bool AlarmManager::ShouldRingAtDate(const time_t& now, const Alarm& alarm) const {
    struct tm tm;
    localtime_r(&now, &tm);
    int weekday = tm.tm_wday;   // 0=周日, 1=周一, ..., 6=周六
    int month = tm.tm_mon + 1;  // 1-12
    int day = tm.tm_mday;       // 1-31

    auto ret = false;
    switch (alarm.repeat_mode) {
        case RepeatMode::ONCE: {
            // 检查是否是设置闹钟的当天（简化处理）
            // 实际应该存储闹钟设置的日期
            ret = true;
            break;
        }
        case RepeatMode::DAILY:
            return true;
        case RepeatMode::WORKDAYS:
            ret = IsWorkday(month, day, weekday);
            break;
        case RepeatMode::HOLIDAYS:
            ret = IsHoliday(month, day, weekday);
            break;
        case RepeatMode::CUSTOM:
            ret = (alarm.repeat_days & (1 << weekday)) != 0;
            break;
        default:
            ret = false;
            break;
    }
    // 打印repeat_days的二进制表示
    ESP_LOGD(
        TAG,
        "ShouldRingAtDate ? %d, alarm time: %02d-%02d-%02d, weekday: %d, repeat mode: %d , repeat "
        "days: %d",
        ret, alarm.hour, alarm.minute, alarm.second, weekday, alarm.repeat_mode, alarm.repeat_days);
    return ret;
}
char* to_string(int64_t val) {
    static char buf[20];
    itoa(val, buf, 10);
    return buf;
}

void AlarmManager::UpdateTimer() {
    std::lock_guard<std::mutex> lock(mutex_);
    UpdateTimerLocked();
}
Alarm* AlarmManager::GetNextRingAtTime(const time_t& now, int64_t& next_ring_time) const {
    Alarm* next_alarm = nullptr;
    int64_t min_delay = INT64_MAX;
    struct tm tm;
    localtime_r(&now, &tm);
    int weekday = tm.tm_wday;   // 0=周日, 1=周一, ..., 6=周六
    int month = tm.tm_mon + 1;  // 1-12
    int day = tm.tm_mday;
    ESP_LOGI(TAG, "today now: %ld, weekday: %d, month: %d, day: %d", ctime(&now), weekday, month,
             day);

    // 找到最早响铃的闹钟
    for (const auto& alarm : alarms_) {
        if (alarm->state != AlarmState::ENABLED && alarm->state != AlarmState::SNOOZED) {
            continue;
        }
        ESP_LOGI(TAG, "alarm: %s(%d-%d-%d), repeat mode: %d, repeat days: %d", alarm->name.c_str(),
                 alarm->hour, alarm->minute, alarm->second, alarm->repeat_mode, alarm->repeat_days);
        for (int i = 0; i < 7; i++) {
            time_t check_time = now + i * 86400;
            // 检查今天是否应该响铃
            if (!ShouldRingAtDate(check_time, *alarm)) {
                continue;
            }

            auto alarm_time = alarm->toTime(check_time);
            auto delay = difftime(alarm_time, now);
            if (delay > 0 && delay < min_delay) {
                min_delay = delay;
                next_alarm = alarm;
            }
        }
    }
    next_ring_time = min_delay;
    return next_alarm;
}

void AlarmManager::UpdateTimerLocked() {
    esp_timer_stop(timer_handle_);

    int64_t min_delay = INT64_MAX;
    time_t original_now = time(nullptr);
    Alarm* next_alarm = GetNextRingAtTime(original_now, min_delay);

    // 如果有闹钟需要响铃，设置定时器
    if (next_alarm != nullptr) {
        StringHelper string_helper;
        auto delay_str = string_helper.MillisecondToString(min_delay * 1000);
        esp_timer_start_once(timer_handle_, min_delay * 1000000);
        auto msg = std::format("Next alarm: {} at {}:{}:{} (after {})", next_alarm->name.c_str(),
                               next_alarm->hour, next_alarm->minute, next_alarm->second, delay_str);
        auto msg_str = msg.c_str();
        display_->SetChatMessage("alarm", msg_str);
        ESP_LOGI(TAG, "%s", msg_str);
    }
}

void AlarmManager::TimerCallback(void* arg) {
    AlarmManager* manager = static_cast<AlarmManager*>(arg);
    manager->OnAlarmTriggered();
}

void AlarmManager::OnAlarmTriggered() {
    std::lock_guard<std::mutex> lock(mutex_);

    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    current_ringing_alarm_ = nullptr;
    // 找到应该响铃的闹钟
    for (auto& alarm : alarms_) {
        if (alarm->state == AlarmState::SNOOZED) {
            // 贪睡定时器触发，直接响铃
        } else if (alarm->state == AlarmState::ENABLED) {
            // 检查时间是否匹配（允许1秒误差）
            if (abs(alarm->hour - tm.tm_hour) > 0 || abs(alarm->minute - tm.tm_min) > 0 ||
                abs(alarm->second - tm.tm_sec) > 1) {
                continue;
            }
            // 检查是否应该今天响铃
            if (!ShouldRingAtDate(now, *alarm)) {
                continue;
            }
        } else {
            continue;
        }

        // 设置为响铃状态
        alarm->state = AlarmState::RINGING;
        alarm->start_ring_time = now;
        current_ringing_alarm_ = alarm;
        is_ringing_ = true;

        RaiseAlarmRinging(*alarm);

        auto msg = std::format("Alarm triggered: {} ({}:{}:{})", alarm->name, alarm->hour,
                               alarm->minute, alarm->second);
        auto msg_str = msg.c_str();
        ESP_LOGI(TAG, "%s", msg_str);
        display_->SetChatMessage("alarm", msg_str);
    }
    if (current_ringing_alarm_ == nullptr) {
        ESP_LOGI(TAG, "No alarm triggered");
    }
}
void AlarmManager::RaiseAlarmRinging(const Alarm& alarm) {
    // 保存当前系统音量
    auto& board = Board::GetInstance();
    auto audio_codec = board.GetAudioCodec();
    original_volume_ = audio_codec->output_volume();

    auto& app = Application::GetInstance();

    StopRingingTask();

    XiaozhiHelper helper;
    while (helper.IsNeedWaitDeviceIdleState()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    app.SetDeviceState(kDeviceStateAlarmClock);
    xTaskCreate(
        [](void* param) {
            vTaskDelay(pdMS_TO_TICKS(500));
            AlarmManager* manager = static_cast<AlarmManager*>(param);
            manager->RingingTask(manager);
        },
        "AlarmRingingTask", 4096, this, tskIDLE_PRIORITY + 1, &ringing_task_handle_);
}

void AlarmManager::StopAlarmRinging(const Alarm& alarm) {
    if (original_volume_ > 0) {
        auto& board = Board::GetInstance();
        auto audio_codec = board.GetAudioCodec();
        audio_codec->SetOutputVolume(original_volume_);
    }

    auto& app = Application::GetInstance();

    StopRingingTask();

    if (app.GetDeviceState() == kDeviceStateAlarmClock) {
        app.SetDeviceState(kDeviceStateIdle);
    }
    ESP_LOGI(TAG, "Alarm stopped: %s", alarm.name.c_str());
}

void AlarmManager::RingingTask(void* data) {
    if (current_ringing_alarm_ == nullptr) {
        ESP_LOGW(TAG, "RingingTask started but no current ringing alarm");
        ringing_task_handle_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    time_t old_now = current_ringing_alarm_->start_ring_time;
    int start_volume = 30;
    int alarm_volume = current_ringing_alarm_->volume;

    auto& board = Board::GetInstance();
    auto audio_codec = board.GetAudioCodec();
    auto& app = Application::GetInstance();
    auto& audio_service = app.GetAudioService();

    auto now = time(nullptr);
    int time_diff = difftime(now, old_now);
    // ？？后自动停止响铃
    int ringing_seconds = 60 * 3;
    stop_ringing_signal_ = false;
    
    while (time_diff < ringing_seconds && !stop_ringing_signal_) {
        auto vol = start_volume + 2 * time_diff * (alarm_volume - start_volume) / ringing_seconds;
        vol = std::min(vol, (int)alarm_volume);
        if (time_diff % 3 == 0 && vol != audio_codec->output_volume()) {
            audio_codec->SetOutputVolume(vol);
        }

        if (audio_service.IsIdle()) {
            app.PlaySound(Lang::Sounds::OGG_ALARM_RING);
        }

        vTaskDelay(pdMS_TO_TICKS(500));
        audio_service.UpdateLastOutputTime();
        now = time(nullptr);
        time_diff = difftime(now, old_now);
    }
    //自己停止自己，不能直接调用StopAlarmRinging，因为它会删除当前任务，导致后续代码无法执行
    ringing_task_handle_ = nullptr;
    if (!stop_ringing_signal_)
        Snooze();
    vTaskDelete(nullptr);
}

void AlarmManager::StopRingingTask() {
    if (!ringing_task_handle_) {
        return;
    }
    auto& app = Application::GetInstance();
    auto& audio_service = app.GetAudioService();
    audio_service.WaitForPlaybackQueueEmpty();
    if (ringing_task_handle_) {
        ringing_task_handle_ = nullptr;
        ESP_LOGI(TAG, "Stopping ringing task");
        vTaskDelete(ringing_task_handle_);
    }
}
bool AlarmManager::IsRinging() const { return is_ringing_; }

Alarm* AlarmManager::GetCurrentRingingAlarm() const { return current_ringing_alarm_; }

bool AlarmManager::OnWakeWordDetected(void* data) {
    auto& app = Application::GetInstance();
    if(app.GetDeviceState() != kDeviceStateAlarmClock) {
        return false;
    }
    stop_ringing_signal_ = true;
    StopRinging();
    ESP_LOGI(TAG, "Wake word detected, stopping alarm ringing");

    XiaozhiHelper helper;
    helper.ReRaiseWakeWordDetectedInTask();
    return true;
}