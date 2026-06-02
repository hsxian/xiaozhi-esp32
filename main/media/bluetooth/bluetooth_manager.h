#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Forward declarations for NimBLE types
struct ble_gap_conn_desc;
class AudioCodec;
class Display;
class McpTool;

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

    // BT stack lifecycle helpers (NimBLE BLE Audio)
    bool InitBluetoothStack();
    void DeinitBluetoothStack();

    // NimBLE task
    static void NimbleTask(void* param);

    // A2DP BLE Audio callbacks
    static void OnA2dpEvent(int event, void* param);
    static void OnAudioData(const uint8_t* data, size_t len);

    // Audio output
    void WriteAudioToCodec(const uint8_t* data, size_t len);

    // MCP tool callbacks
    std::string OnToolGetStatus();
    std::string OnToolControl(bool enable);
    std::string OnToolSetVolume(int volume);

    // State
    bool initialized_ = false;
    bool bluetooth_started_ = false;
    std::atomic<bool> enabled_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> audio_streaming_{false};

    // Connected device info (protected by mutex_)
    std::string device_name_;
    uint8_t peer_bda_[6];  // BLE address (6 bytes)

    // Audio config received from BLE Audio
    int audio_sample_rate_ = 44100;
    int audio_channels_ = 2;

    // Cached pointers
    AudioCodec* audio_codec_ = nullptr;
    Display* display_ = nullptr;

    // Volume level for Bluetooth audio (0-100)
    int bt_volume_ = 70;
};