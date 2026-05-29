#pragma once
#include <functional>
#include <map>
#include <mutex>
#include <vector>

class EventListener {
public:
    using EventCallback = std::function<bool(void*)>;
    EventListener() = default;
    ~EventListener() = default;

    int AddEventListener(EventCallback callback);
    void RemoveEventListener(int listener_id);
    const std::vector<EventCallback> GetListeners();

private:
    int next_listener_id_{0};
    std::mutex mutex_;
    std::map<int, EventCallback> listeners_;
};