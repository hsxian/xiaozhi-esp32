#pragma once

#include <esp_timer.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "alarm.h"

class McpTool;

// 闹钟回调函数类型
using AlarmCallback = std::function<void(const Alarm& alarm)>;
using AlarmStopCallback = std::function<void(const Alarm& alarm)>;

// 闹钟管理器类
class AlarmManager {
public:
    static AlarmManager& GetInstance();
    ~AlarmManager();

    // 禁止拷贝
    AlarmManager(const AlarmManager&) = delete;
    AlarmManager& operator=(const AlarmManager&) = delete;

    // 初始化
    void Initialize();

    // 添加闹钟
    bool AddAlarm(const Alarm& alarm);

    // 删除闹钟
    bool RemoveAlarm(const std::string& alarm_id);
    bool RemoveAllAlarms();

    // 获取所有闹钟
    std::vector<Alarm> GetAlarms() const;

    // 获取单个闹钟
    bool GetAlarm(const std::string& alarm_id, Alarm& out_alarm) const;

    // 更新闹钟
    bool UpdateAlarm(const Alarm& alarm);

    // 启用/禁用闹钟
    bool SetAlarmEnabled(const std::string& alarm_id, bool enabled);

    // 停止响铃
    void StopRinging();

    // 贪睡
    void Snooze();

    // 设置闹钟触发回调
    void SetAlarmCallback(AlarmCallback callback);

    // 设置闹钟停止回调
    void SetAlarmStopCallback(AlarmStopCallback callback);

    // 添加节假日
    void AddHoliday(const Holiday& holiday);

    // 获取节假日列表
    std::vector<Holiday> GetHolidays() const;

    // 检查某天是否是节假日
    bool IsHoliday(int month, int day, int weekday) const;

    // 检查某天是否是工作日
    bool IsWorkday(int month, int day, int weekday) const;

    // 检查闹钟今天是否应该响铃
    bool ShouldRingToday(const Alarm& alarm) const;

    // 加载节假日配置
    void LoadHolidays();

    // 生成MCP服务器工具
    void GenerateMcpServerTools(std::vector<McpTool*>& tools);

    bool IsRinging() const;

    void GetCurrentRingingAlarm(Alarm& out_alarm) const;

private:
    AlarmManager();

    // 加载闹钟配置（从NVS）
    void LoadAlarms();

    // 保存闹钟配置（到NVS）
    void SaveAlarms();

    // 计算下一次响铃时间
    int64_t CalculateNextRingTime(const Alarm& alarm) const;

    // 更新定时器
    void UpdateTimer();

    void UpdateTimerLocked();
    bool RemoveAlarmLocked(const std::string& alarm_id);
    // 定时器回调函数
    static void TimerCallback(void* arg);

    // 闹钟触发处理
    void OnAlarmTriggered();

    void CallAlarmCallback(const Alarm& alarm);
    void CallAlarmStopCallback(const Alarm& alarm);
    std::vector<Alarm> alarms_;
    std::vector<Holiday> holidays_;
    AlarmCallback alarm_callback_ = nullptr;
    AlarmStopCallback alarm_stop_callback_ = nullptr;
    esp_timer_handle_t timer_handle_;
    Alarm current_ringing_alarm_;
    bool is_ringing_;
    mutable std::mutex mutex_;
    // 闹钟响起之前的系统音量
    int original_volume_;
};