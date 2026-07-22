#pragma once
#include <functional>
class XiaozhiHelper {
public:
    bool IsNeedWaitDeviceIdleState() const;
    void ReRaiseWakeWordDetectedInTask(const std::function<void()>& callback = nullptr) const;
};