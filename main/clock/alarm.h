#pragma once

#include <time.h>
#include <string>
#include <vector>

struct cJSON;
// 重复模式枚举
enum class RepeatMode {
    ONCE,      // 单次响铃
    DAILY,     // 每天
    WORKDAYS,  // 工作日
    HOLIDAYS,  // 节假日
    CUSTOM     // 自定义重复日期
};

// 闹钟状态枚举
enum class AlarmState {
    DISABLED,  // 禁用
    ENABLED,   // 启用
    RINGING,   // 响铃中
    SNOOZED    // 贪睡中
};

// 闹钟结构体
// 绝对时间闹钟，如果是要相对时间，需要先获取当前时间（比如现在是10:00:00），加上设置值（比如10分钟），应设置为10:10:00
struct Alarm {
    std::string id;          // 闹钟唯一ID
    std::string name;        // 闹钟名称
    int hour;                // 小时 (0-23)
    int minute;              // 分钟 (0-59)
    int second;              // 秒 (0-59)
    int volume;              // 音量 (0-100)
    RepeatMode repeat_mode;  // 重复模式
    int repeat_days;         // 自定义重复日期（周日到周六，共7天,每个位表示一个天）最大值为127
    AlarmState state;        // 闹钟状态
    int snooze_duration;     // 贪睡时长（分钟），默认5分钟
    int snooze_count;        // 剩余贪睡次数，-1表示无限次
    time_t start_ring_time;   // 开始响铃时间
    // 默认构造函数
    Alarm();        

    // 带参数构造函数
    Alarm(const std::string& alarm_id, const std::string& alarm_name, int h, int m, int s = 0,
          int vol = 80, RepeatMode mode = RepeatMode::ONCE, int repeat_days = 0);
      
    // 转换为JSON字符串
    std::string ToJson() const;
    void ToJson(cJSON* root) const;
    static std::string ToJsonArray(std::vector<Alarm>& alarms);
    static bool findByName(std::vector<Alarm>& alarms, const std::string& name,
                           std::vector<Alarm>& found_alarms);

    // 从JSON字符串解析
    bool FromJson(const std::string& json);
    time_t toTime(const time_t& now) const;
};

// 节假日结构体
struct Holiday {
    int month;         // 月份 (1-12)
    int day;           // 日期 (1-31)
    std::string name;  // 节假日名称
    // 是否休息，true表示休息，false表示不休息
    bool is_offday;
    Holiday(int m, int d, const std::string& n, bool is_offday = true)
        : month(m), day(d), name(n), is_offday(is_offday) {}
};