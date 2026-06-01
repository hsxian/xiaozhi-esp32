#include "bluetooth_manager.h"

#include <cstring>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "display.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "mcp_server.h"

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
    memset(peer_bda_, 0, sizeof(esp_bd_addr_t));
    ESP_LOGI(TAG, "BluetoothManager constructed");
}

BluetoothManager::~BluetoothManager() {
    if (bluetooth_started_) {
        DeinitBluetoothStack();
    }
}

void BluetoothManager::Initialize() {
    if (initialized_) return;
    initialized_ = true;
    ESP_LOGI(TAG, "BluetoothManager initialized (BT stack NOT started)");
}

// ============================================================================
// State Queries
// ============================================================================

std::string BluetoothManager::GetConnectedDeviceName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return device_name_;
}

// ============================================================================
// BT Stack Lifecycle
// ============================================================================

bool BluetoothManager::InitBluetoothStack() {
    if (bluetooth_started_) return true;

    // Step 1: Check if Bluedroid host is already initialized (e.g. by Blufi).
    // If the host is up, we reuse it; otherwise we initialize it ourselves.
    const uint8_t* bda = esp_bt_dev_get_address();
    bool host_already_up = false;
    if (bda != nullptr) {
        // Check if address is non-zero (valid address means host is initialized)
        for (int i = 0; i < ESP_BD_ADDR_LEN; i++) {
            if (bda[i] != 0) {
                host_already_up = true;
                break;
            }
        }
    }

    if (!host_already_up) {
        // Bluedroid host is not initialized. Initialize it.
        esp_err_t ret = esp_bluedroid_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_bluedroid_init failed: %s", esp_err_to_name(ret));
            return false;
        }
        ret = esp_bluedroid_enable();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_bluedroid_enable failed: %s", esp_err_to_name(ret));
            esp_bluedroid_deinit();
            return false;
        }
        host_owned_by_us_ = true;
        ESP_LOGI(TAG, "Bluedroid host initialized by BluetoothManager");
    } else {
        host_owned_by_us_ = false;
        ESP_LOGI(TAG, "Bluedroid host already running (e.g. Blufi), reusing");
    }

    // Step 2: Register GAP callback for classic BT events
    esp_bt_gap_register_callback([](esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
        // Log discovery and ACL events for debugging
        if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT) {
            ESP_LOGD(TAG, "GAP discovery state changed: %d", param->disc_st_chg.state);
        } else if (event == ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT) {
            ESP_LOGD(TAG, "GAP ACL connection complete: status=%d", param->acl_conn_cmpl_stat.stat);
        } else if (event == ESP_BT_GAP_CFM_REQ_EVT) {
            // Accept pairing confirmation automatically
            ESP_LOGI(TAG, "GAP pairing confirmation request, accepting");
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        }
    });

    // Step 3: Set scan mode to connectable (not discoverable to save power after connection)
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    // Step 4: Set device name
    esp_bt_gap_set_device_name("Xiaozhi-Speaker");

    // Step 5: Initialize A2DP sink
    if (!StartA2dpSink()) {
        if (host_owned_by_us_) {
            esp_bluedroid_disable();
            esp_bluedroid_deinit();
            host_owned_by_us_ = false;
        }
        return false;
    }

    bluetooth_started_ = true;
    ESP_LOGI(TAG, "Bluetooth stack started successfully");
    return true;
}

void BluetoothManager::DeinitBluetoothStack() {
    if (!bluetooth_started_) return;

    StopA2dpSink();

    // Only deinit bluedroid host if we initialized it
    if (host_owned_by_us_) {
        esp_bluedroid_disable();
        esp_bluedroid_deinit();
        host_owned_by_us_ = false;
        ESP_LOGI(TAG, "Bluedroid host deinitialized");
    }

    bluetooth_started_ = false;
}

// ============================================================================
// A2DP Sink Initialization
// ============================================================================

bool BluetoothManager::StartA2dpSink() {
    // Register the A2DP event callback
    esp_err_t ret = esp_a2d_register_callback(A2dpCallback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_a2d_register_callback failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Register the audio data callback (receives decoded PCM after SBC)
    ret = esp_a2d_sink_register_audio_data_callback(AudioDataCallback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_a2d_sink_register_audio_data_callback failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Initialize A2DP sink
    ret = esp_a2d_sink_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_a2d_sink_init failed: %s", esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG, "A2DP sink initialized, waiting for connection...");
    return true;
}

void BluetoothManager::StopA2dpSink() {
    if (connected_.load()) {
        // Disconnect any active connection
        esp_bd_addr_t bda;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            memcpy(bda, peer_bda_, sizeof(esp_bd_addr_t));
        }
        esp_a2d_sink_disconnect(bda);
    }
    esp_a2d_sink_deinit();
    audio_streaming_ = false;
    connected_ = false;
    ESP_LOGI(TAG, "A2DP sink stopped");
}

// ============================================================================
// A2DP Event Callback
// ============================================================================

void BluetoothManager::A2dpCallback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t* param) {
    auto& mgr = GetInstance();

    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT:
            mgr.OnConnectionStateChanged(param);
            break;
        case ESP_A2D_AUDIO_STATE_EVT:
            mgr.OnAudioStateChanged(param);
            break;
        case ESP_A2D_AUDIO_CFG_EVT:
            mgr.OnAudioConfigChanged(param);
            break;
        case ESP_A2D_PROF_STATE_EVT:
            ESP_LOGI(TAG, "A2DP profile state: %d", param->a2d_prof_stat.init_state);
            break;
        case ESP_A2D_SNK_PSC_CFG_EVT:
            ESP_LOGI(TAG, "A2DP SNK PSC configured");
            break;
        default:
            ESP_LOGD(TAG, "A2DP event: %d", event);
            break;
    }
}

void BluetoothManager::OnConnectionStateChanged(esp_a2d_cb_param_t* param) {
    auto conn_state = param->conn_stat.state;
    ESP_LOGI(TAG, "A2DP connection state: %d", conn_state);

    if (conn_state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
        connected_ = true;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            memcpy(peer_bda_, param->conn_stat.remote_bda, sizeof(esp_bd_addr_t));
            // Remote device name is obtained via EIR data during discovery.
            // For now, use BD_ADDR as identification.
            char bda_str[18];
            snprintf(bda_str, sizeof(bda_str), ESP_BD_ADDR_STR,
                     ESP_BD_ADDR_HEX(peer_bda_));
            device_name_ = std::string(bda_str);
        }
        ESP_LOGI(TAG, "Connected to device: [" ESP_BD_ADDR_STR "]",
                 ESP_BD_ADDR_HEX(peer_bda_));

        // Safely update display from BTU task via Schedule
        if (display_) {
            Display* d = display_;
            std::string name = GetConnectedDeviceName();
            Application::GetInstance().Schedule([d, name]() {
                d->SetChatMessage("bluetooth", ("BT Connected: " + name).c_str());
            });
        }
    } else if (conn_state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
        connected_ = false;
        audio_streaming_ = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            device_name_.clear();
            memset(peer_bda_, 0, sizeof(esp_bd_addr_t));
        }
        ESP_LOGI(TAG, "A2DP disconnected");

        if (display_) {
            Display* d = display_;
            Application::GetInstance().Schedule([d]() {
                d->SetChatMessage("bluetooth", "BT Disconnected");
            });
        }
    }
}

void BluetoothManager::OnAudioStateChanged(esp_a2d_cb_param_t* param) {
    auto audio_state = param->audio_stat.state;
    ESP_LOGI(TAG, "A2DP audio state: %d", audio_state);

    if (audio_state == ESP_A2D_AUDIO_STATE_STARTED) {
        audio_streaming_ = true;
        if (audio_codec_) {
            audio_codec_->EnableOutput(true);
            audio_codec_->SetOutputVolume(bt_volume_);
        }
        ESP_LOGI(TAG, "Bluetooth audio streaming started");
    } else if (audio_state == ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND ||
               audio_state == ESP_A2D_AUDIO_STATE_STOPPED) {
        audio_streaming_ = false;
        ESP_LOGI(TAG, "Bluetooth audio streaming stopped/suspended");
    }
}

void BluetoothManager::OnAudioConfigChanged(esp_a2d_cb_param_t* param) {
    // ESP-A2DP SBC decoder configuration
    // The audio_cfg contains the negotiated SBC codec parameters
    auto& mcc = param->audio_cfg.mcc;
    ESP_LOGI(TAG, "Audio config: codec_type=%d", mcc.type);

    if (mcc.type == ESP_A2D_MCT_SBC) {
        auto& sbc = mcc.cie.sbc_info;
        // Parse SBC sampling frequency
        if (sbc.samp_freq & ESP_A2D_SBC_CIE_SF_48K)
            audio_sample_rate_ = 48000;
        else if (sbc.samp_freq & ESP_A2D_SBC_CIE_SF_44K)
            audio_sample_rate_ = 44100;
        else if (sbc.samp_freq & ESP_A2D_SBC_CIE_SF_32K)
            audio_sample_rate_ = 32000;
        else if (sbc.samp_freq & ESP_A2D_SBC_CIE_SF_16K)
            audio_sample_rate_ = 16000;
        else
            audio_sample_rate_ = 44100;  // default

        // Parse channel mode
        if (sbc.ch_mode & ESP_A2D_SBC_CIE_CH_MODE_STEREO ||
            sbc.ch_mode & ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO)
            audio_channels_ = 2;
        else if (sbc.ch_mode & ESP_A2D_SBC_CIE_CH_MODE_MONO)
            audio_channels_ = 1;
        else
            audio_channels_ = 2;  // default

        ESP_LOGI(TAG, "Audio config: sample_rate=%d, channels=%d",
                 audio_sample_rate_, audio_channels_);
    }
}

// ============================================================================
// Audio Data Callback
// ============================================================================

void BluetoothManager::AudioDataCallback(esp_a2d_conn_hdl_t conn_hdl,
                                          esp_a2d_audio_buff_t* audio_buf) {
    auto& mgr = GetInstance();
    if (mgr.audio_codec_ && mgr.audio_streaming_.load() && audio_buf && audio_buf->data) {
        mgr.WriteAudioToCodec(audio_buf->data, audio_buf->data_len);
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
        [this](const PropertyList& properties) -> ReturnValue {
            return OnToolGetStatus();
        });
    tools.push_back(tool);

    // Tool 2: Enable/disable Bluetooth speaker mode
    tool = new McpTool(
        "self.bluetooth.control",
        "Enable or disable the Bluetooth speaker mode. When enabled, the device becomes "
        "discoverable and connectable as an A2DP Bluetooth speaker. When disabled, "
        "Bluetooth audio is stopped and any active connection is terminated.",
        PropertyList({
            Property("enable", kPropertyTypeBoolean)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            bool enable = properties["enable"].value<bool>();
            return OnToolControl(enable);
        });
    tools.push_back(tool);

    // Tool 3: Volume control
    tool = new McpTool(
        "self.bluetooth.volume",
        "Set the Bluetooth audio playback volume. Volume range is 0 to 100.",
        PropertyList({
            Property("volume", kPropertyTypeInteger, 70, 0, 100)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            int volume = properties["volume"].value<int>();
            return OnToolSetVolume(volume);
        });
    tools.push_back(tool);
}

std::string BluetoothManager::OnToolGetStatus() {
    std::lock_guard<std::mutex> lock(mutex_);

    cJSON* json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "enabled", enabled_.load());
    cJSON_AddBoolToObject(json, "connected", connected_.load());
    cJSON_AddBoolToObject(json, "audio_streaming", audio_streaming_.load());
    cJSON_AddStringToObject(json, "device_name",
                            connected_.load() ? device_name_.c_str() : "");
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
            return "Failed to start Bluetooth stack. Check logs for details.";
        }
        enabled_ = true;

        if (display_) {
            Display* d = display_;
            Application::GetInstance().Schedule([d]() {
                d->SetChatMessage("bluetooth", "BT Speaker enabled, waiting for connection");
            });
        }
        ESP_LOGI(TAG, "Bluetooth speaker enabled. Device name: Xiaozhi-Speaker");
        return "Bluetooth speaker enabled. Device name: Xiaozhi-Speaker. Waiting for phone to connect.";
    } else {
        DeinitBluetoothStack();
        enabled_ = false;

        if (display_) {
            Display* d = display_;
            Application::GetInstance().Schedule([d]() {
                d->SetChatMessage("bluetooth", "BT Speaker disabled");
            });
        }
        ESP_LOGI(TAG, "Bluetooth speaker disabled");
        return "Bluetooth speaker disabled.";
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
