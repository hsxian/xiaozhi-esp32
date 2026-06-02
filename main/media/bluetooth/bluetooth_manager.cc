#include "bluetooth_manager.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "display.h"
#include "esp_log.h"
#include "mcp_server.h"
#include "boards/bread-compact-wifi/config.h"

// a2dpSinkHfpClient includes
#include "a2dpSinkHfpHf.h"

#define TAG "BluetoothManager"

// ============================================================================
// Singleton & Lifecycle
// ============================================================================

BluetoothManager& BluetoothManager::GetInstance() {
    static BluetoothManager instance;
    return instance;
}

BluetoothManager::BluetoothManager()
    : audio_codec_(Board::GetInstance().GetAudioCodec()),
      display_(Board::GetInstance().GetDisplay()) {
    // memset(peer_bda_, 0, sizeof(esp_bd_addr_t));
    Initialize();
    ESP_LOGI(TAG, "BluetoothManager constructed (NimBLE BLE Audio)");
}

BluetoothManager::~BluetoothManager() {
    if (bluetooth_started_) {
        DeinitBluetoothStack();
    }
}

void BluetoothManager::Initialize() {
    if (initialized_)
        return;
    initialized_ = true;

    a2dpSinkHfpHf_config_t config = {
        .device_name = "ESP32-Speaker",
        .i2s_tx_bck = AUDIO_I2S_SPK_GPIO_BCLK,    // Speaker pins
        .i2s_tx_ws = AUDIO_I2S_SPK_GPIO_LRCK,
        .i2s_tx_dout = AUDIO_I2S_SPK_GPIO_DOUT,
        .i2s_rx_bck = AUDIO_I2S_MIC_GPIO_DIN,    // Microphone pins (for calls)
        .i2s_rx_ws = AUDIO_I2S_MIC_GPIO_WS,
        .i2s_rx_din = AUDIO_I2S_MIC_GPIO_DIN
    };

    auto ret = a2dpSinkHfpHf_init(&config);

    ESP_LOGI(TAG, "BluetoothManager initialized (BT stack NOT started, ret=%d)", ret);
}

// ============================================================================
// State Queries
// ============================================================================

std::string BluetoothManager::GetConnectedDeviceName() const {
    
    return device_name_;
}

// ============================================================================
// BT Stack Lifecycle (NimBLE BLE Audio)
// ============================================================================

bool BluetoothManager::InitBluetoothStack() {
    if (bluetooth_started_)
        return true;

   
    return true;
}

void BluetoothManager::DeinitBluetoothStack() {
    if (!bluetooth_started_)
        return;


    bluetooth_started_ = false;
    ESP_LOGI(TAG, "NimBLE BLE Audio stack stopped");
}

// NimBLE host task
void BluetoothManager::NimbleTask(void* param) {
}

// ============================================================================
// A2DP BLE Audio Event Handler
// ============================================================================

// void BluetoothManager::OnA2dpEvent(a2dp_event_t event, void* param) {
    // auto& mgr = GetInstance();

    // switch (event) {
    //     case A2DP_EVENT_CONNECTED: {
    //         a2dp_conn_param_t* conn_param = (a2dp_conn_param_t*)param;
    //         mgr.connected_ = true;
    //         {
    //             std::lock_guard<std::mutex> lock(mgr.mutex_);
    //             memcpy(mgr.peer_bda_, conn_param->bda, sizeof(esp_bd_addr_t));
    //             mgr.device_name_ = conn_param->name ? conn_param->name : "Unknown";
    //         }
    //         ESP_LOGI(TAG, "BLE Audio connected: %s", mgr.device_name_.c_str());

    //         if (mgr.display_) {
    //             Display* d = mgr.display_;
    //             std::string name = mgr.GetConnectedDeviceName();
    //             Application::GetInstance().Schedule([d, name]() {
    //                 d->SetChatMessage("bluetooth", ("BT Connected: " + name).c_str());
    //             });
    //         }
    //         break;
    //     }
    //     case A2DP_EVENT_DISCONNECTED: {
    //         mgr.connected_ = false;
    //         mgr.audio_streaming_ = false;
    //         {
    //             std::lock_guard<std::mutex> lock(mgr.mutex_);
    //             mgr.device_name_.clear();
    //             memset(mgr.peer_bda_, 0, sizeof(esp_bd_addr_t));
    //         }
    //         ESP_LOGI(TAG, "BLE Audio disconnected");

    //         if (mgr.display_) {
    //             Display* d = mgr.display_;
    //             Application::GetInstance().Schedule(
    //                 [d]() { d->SetChatMessage("bluetooth", "BT Disconnected"); });
    //         }
    //         break;
    //     }
    //     case A2DP_EVENT_AUDIO_STARTED: {
    //         mgr.audio_streaming_ = true;
    //         if (mgr.audio_codec_) {
    //             mgr.audio_codec_->EnableOutput(true);
    //             mgr.audio_codec_->SetOutputVolume(mgr.bt_volume_);
    //         }
    //         ESP_LOGI(TAG, "BLE Audio streaming started");
    //         break;
    //     }
    //     case A2DP_EVENT_AUDIO_STOPPED: {
    //         mgr.audio_streaming_ = false;
    //         ESP_LOGI(TAG, "BLE Audio streaming stopped");
    //         break;
    //     }
    //     case A2DP_EVENT_AUDIO_CONFIG: {
    //         a2dp_audio_config_t* config = (a2dp_audio_config_t*)param;
    //         mgr.audio_sample_rate_ = config->sample_rate;
    //         mgr.audio_channels_ = config->channels;
    //         ESP_LOGI(TAG, "Audio config: sample_rate=%d, channels=%d", mgr.audio_sample_rate_,
    //                  mgr.audio_channels_);
    //         break;
    //     }
    //     default:
    //         ESP_LOGD(TAG, "A2DP event: %d", event);
    //         break;
    // }
// }

// ============================================================================
// Audio Data Callback
// ============================================================================

void BluetoothManager::OnAudioData(const uint8_t* data, size_t len) {
    auto& mgr = GetInstance();
    if (mgr.audio_codec_ && mgr.audio_streaming_.load() && data) {
        mgr.WriteAudioToCodec(data, len);
    }
}

void BluetoothManager::WriteAudioToCodec(const uint8_t* data, size_t len) {
    // audio_buf->data contains int16_t PCM samples in S16LE format
    // audio_buf->data_len is in bytes; convert to sample count
    int samples = len / sizeof(int16_t);
    audio_codec_->OutputData(reinterpret_cast<const int16_t*>(data), samples);
}

// ============================================================================
// MCP Tool Implementations
// ============================================================================

void BluetoothManager::GenerateMcpServerTools(std::vector<McpTool*>& tools) {
    // Tool 1: Get Bluetooth status
    auto tool = new McpTool(
        "self.bluetooth.status",
        "Get the current Bluetooth speaker status. Returns whether Bluetooth is enabled, "
        "connection state, the connected device name (if any), audio streaming state, and volume.",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue { return OnToolGetStatus(); });
    tools.push_back(tool);

    // Tool 2: Enable/disable Bluetooth speaker mode
    tool = new McpTool(
        "self.bluetooth.control",
        "Enable or disable the Bluetooth speaker mode. When enabled, the device becomes "
        "discoverable and connectable as a BLE Audio speaker. When disabled, "
        "Bluetooth audio is stopped and any active connection is terminated.",
        PropertyList({Property("enable", kPropertyTypeBoolean)}),
        [this](const PropertyList& properties) -> ReturnValue {
            bool enable = properties["enable"].value<bool>();
            return OnToolControl(enable);
        });
    tools.push_back(tool);

    // Tool 3: Volume control
    tool = new McpTool("self.bluetooth.volume",
                       "Set the Bluetooth audio playback volume. Volume range is 0 to 100.",
                       PropertyList({Property("volume", kPropertyTypeInteger, 70, 0, 100)}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           int volume = properties["volume"].value<int>();
                           return OnToolSetVolume(volume);
                       });
    tools.push_back(tool);
}

std::string BluetoothManager::OnToolGetStatus() {
    

    cJSON* json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "enabled", enabled_.load());
    cJSON_AddBoolToObject(json, "connected", connected_.load());
    cJSON_AddBoolToObject(json, "audio_streaming", audio_streaming_.load());
    cJSON_AddStringToObject(json, "device_name", connected_.load() ? device_name_.c_str() : "");
    cJSON_AddNumberToObject(json, "volume", bt_volume_);

    char* json_str = cJSON_PrintUnformatted(json);
    std::string result(json_str);
    cJSON_free(json_str);
    cJSON_Delete(json);
    return result;
}

std::string BluetoothManager::OnToolControl(bool enable) {
    if (enable == enabled_.load()) {
        return enabled_.load() ? "Bluetooth speaker is already enabled"
                               : "Bluetooth speaker is already disabled";
    }

    if (enable) {
        if (!InitBluetoothStack()) {
            return "Failed to start BLE Audio stack. Check logs for details.";
        }
        enabled_ = true;

        if (display_) {
            Display* d = display_;
            Application::GetInstance().Schedule([d]() {
                d->SetChatMessage("bluetooth", "BT Speaker enabled, waiting for connection");
            });
        }
        ESP_LOGI(TAG, "BLE Audio speaker enabled. Device name: Xiaozhi-Speaker");
        return "BLE Audio speaker enabled. Device name: Xiaozhi-Speaker. Waiting for phone to "
               "connect.";
    } else {
        DeinitBluetoothStack();
        enabled_ = false;

        if (display_) {
            Display* d = display_;
            Application::GetInstance().Schedule(
                [d]() { d->SetChatMessage("bluetooth", "BT Speaker disabled"); });
        }
        ESP_LOGI(TAG, "BLE Audio speaker disabled");
        return "BLE Audio speaker disabled.";
    }
}

std::string BluetoothManager::OnToolSetVolume(int volume) {
    bt_volume_ = volume;
    if (audio_streaming_.load() && audio_codec_) {
        audio_codec_->SetOutputVolume(volume);
    }
    ESP_LOGI(TAG, "Bluetooth volume set to %d", volume);
    return "Bluetooth volume set to " + std::to_string(volume);
}