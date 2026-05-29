#include "event_listener.h"

int EventListener::AddEventListener(EventCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    int listener_id = next_listener_id_++;
    listeners_[listener_id] = callback;
    return listener_id;
}

void EventListener::RemoveEventListener(int listener_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    listeners_.erase(listener_id);
}

const std::vector<EventListener::EventCallback> EventListener::GetListeners() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<EventListener::EventCallback> callbacks;
    for (const auto& pair : listeners_) {
        callbacks.push_back(pair.second);
    }
    return callbacks;
}
