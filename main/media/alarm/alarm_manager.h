#pragma once

#include <esp_timer.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "alarm.h"
#include <freertos/FreeRTOS.h>

class McpTool;
class Display;


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
    std::vector<Alarm*> GetAlarms() const;

    // 获取单个闹钟
    Alarm* GetAlarm(const std::string& alarm_id) const;

    // 更新闹钟
    bool UpdateAlarm(const Alarm& alarm);

    // 启用/禁用闹钟
    bool SetAlarmEnabled(const std::string& alarm_id, bool enabled);

    // 停止响铃
    void StopRinging();

    // 贪睡
    void Snooze();

    // 添加节假日
    void AddHoliday(const Holiday& holiday);

    // 获取节假日列表
    std::vector<Holiday> GetHolidays() const;

    // 检查某天是否是节假日
    bool IsHoliday(int month, int day, int weekday) const;

    // 检查某天是否是工作日
    bool IsWorkday(int month, int day, int weekday) const;

    // 检查闹钟在指定时间点是否应该响铃
    bool ShouldRingAtDate(const time_t& now, const Alarm& alarm) const;
    Alarm* GetNextRingAtTime(const time_t& now,  int64_t& next_ring_time) const;

    // 加载节假日配置
    void LoadHolidays();

    // 生成MCP服务器工具
    void GenerateMcpServerTools(std::vector<McpTool*>& tools);

    bool IsRinging() const;

    Alarm* GetCurrentRingingAlarm() const;

private:
    AlarmManager();

    // 加载闹钟配置（从NVS）
    void LoadAlarms();

    // 保存闹钟配置（到NVS）
    void SaveAlarms();

    // 更新定时器
    void UpdateTimer();

    void UpdateTimerLocked();
    bool RemoveAlarmLocked(const std::string& alarm_id);
    // 定时器回调函数
    static void TimerCallback(void* arg);

    // 闹钟触发处理
    void OnAlarmTriggered();

    void RaiseAlarmRinging(const Alarm& alarm);
    void StopAlarmRinging(const Alarm& alarm);
    bool OnWakeWordDetected(void* data);
    void RingingTask(void* data);
    void StopRingingTask();

    std::vector<Alarm*> alarms_;
    std::vector<Holiday> holidays_;
    esp_timer_handle_t timer_handle_;
    TaskHandle_t ringing_task_handle_{nullptr};
    Alarm* current_ringing_alarm_{nullptr};
    bool is_ringing_;
    bool stop_ringing_signal_ = false;
    mutable std::mutex mutex_;
    // 闹钟响起之前的系统音量
    int original_volume_;
    Display* display_;
};