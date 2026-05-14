#pragma once

#include "application.h"

class AlarmEventConfig {
private:
    AlarmEventConfig(/* args */);
    ~AlarmEventConfig();

public:

    static AlarmEventConfig& GetInstance();

    bool HandleAlarmRingingEvent(bool& aborted, std::unique_ptr<Protocol>& protocol);
    bool HandleWakeWordDetected(const std::string& wake_word, std::unique_ptr<Protocol>& protocol);

    void SetDeviceState();
};
