#include "xiaozhi_helper.h"
#include "application.h"

bool XiaozhiHelper::IsNeedWaitDeviceIdleState() const {
    auto& app = Application::GetInstance();
    auto current_state = app.GetDeviceState();
    // 等待说话中状态结束
    if (current_state == kDeviceStateSpeaking)
        return true;
    // 状态转换：聆听中-》待机状态-》播放音乐
    if (current_state == kDeviceStateListening)
        app.ToggleChatState();
    return app.GetDeviceState() != kDeviceStateIdle;  // 检查更新后的状态
}

void XiaozhiHelper::ReRaiseWakeWordDetectedInTask(const std::function<void()>& callback) const {
    std::function<void()>* callback_ptr = new std::function<void()>(callback);
    xTaskCreate(
        [](void* pvParameters) {
            std::function<void()>* callback = static_cast<std::function<void()>*>(pvParameters);
            vTaskDelay(pdMS_TO_TICKS(1500));
            auto& app = Application::GetInstance();
            app.ClearEventFromGroup(MAIN_EVENT_WAKE_WORD_DETECTED);
            vTaskDelay(pdMS_TO_TICKS(1000));
            app.AppendEventToGroup(MAIN_EVENT_WAKE_WORD_DETECTED);
            if (callback && *callback)
                (*callback)();
            delete callback;
            vTaskDelete(nullptr);
        },
        "OnWakeWordDetectedTask", 2048, callback_ptr, 5, nullptr);
}