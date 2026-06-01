#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include "esp_a2dp_api.h"
#include "esp_bt_device.h"

class McpTool;
class AudioCodec;
class Display;

class BluetoothManager {
public:
    static BluetoothManager& GetInstance();

    BluetoothManager(const BluetoothManager&) = delete;
    BluetoothManager& operator=(const BluetoothManager&) = delete;

    // Lifecycle
    void Initialize();

    // State queries
    bool IsConnected() const { return connected_.load(); }
    bool IsEnabled() const { return enabled_.load(); }
    bool IsAudioStreaming() const { return audio_streaming_.load(); }
    std::string GetConnectedDeviceName() const;

    // MCP tools
    void GenerateMcpServerTools(std::vector<McpTool*>& tools);

private:
    BluetoothManager();
    ~BluetoothManager();

    // BT stack lifecycle helpers
    bool InitBluetoothStack();
    void DeinitBluetoothStack();
    bool StartA2dpSink();
    void StopA2dpSink();

    // A2DP callbacks (static trampolines, called in BTU task context)
    static void A2dpCallback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t* param);
    static void AudioDataCallback(esp_a2d_conn_hdl_t conn_hdl, esp_a2d_audio_buff_t* audio_buf);

    // Internal state handlers (called from A2dpCallback)
    void OnConnectionStateChanged(esp_a2d_cb_param_t* param);
    void OnAudioStateChanged(esp_a2d_cb_param_t* param);
    void OnAudioConfigChanged(esp_a2d_cb_param_t* param);

    // MCP tool callbacks
    std::string OnToolGetStatus();
    std::string OnToolControl(bool enable);
    std::string OnToolSetVolume(int volume);

    // Audio output
    void WriteAudioToCodec(const uint8_t* data, size_t len);

    // State
    bool initialized_ = false;
    bool bluetooth_started_ = false;
    bool host_owned_by_us_ = false;  // true if we called esp_bluedroid_init()
    std::atomic<bool> enabled_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> audio_streaming_{false};

    // Connected device info (protected by mutex_)
    mutable std::mutex mutex_;
    std::string device_name_;
    esp_bd_addr_t peer_bda_;

    // Audio config received from A2DP
    int audio_sample_rate_ = 44100;
    int audio_channels_ = 2;

    // Cached pointers
    AudioCodec* audio_codec_ = nullptr;
    Display* display_ = nullptr;

    // Volume level for Bluetooth audio (0-100)
    int bt_volume_ = 70;
};
