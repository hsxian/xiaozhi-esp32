#include "http_stream.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_tls.h"

#define TAG "HttpStream"

HttpStream::HttpStream() {
    data_queue_ = xQueueCreate(QUEUE_SIZE, sizeof(DataChunk));
    if (!data_queue_) {
        ESP_LOGE(TAG, "Failed to create data queue");
    }
}

HttpStream::~HttpStream() {
    StopRequest();
    if (data_queue_) {
        vQueueDelete(data_queue_);
        data_queue_ = nullptr;
    }
}

esp_err_t HttpStream::http_event_handler(esp_http_client_event_t* evt) {
    // ESP_LOGI(TAG, "http_event_handler, event_id=%d", evt->event_id);
    auto stream = static_cast<HttpStream*>(evt->user_data);
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA: {
            DataChunk chunk;
            chunk.data = new uint8_t[evt->data_len];
            memcpy(chunk.data, evt->data, evt->data_len);
            chunk.size = evt->data_len;
            chunk.status = DataStatus::kNormal;
            stream->SendData(chunk);
            stream->download_bytes_received_ += evt->data_len;
            // ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d, download_bytes_received=%d",
            // evt->data_len,
            //          stream->download_bytes_received_);

        } break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
            // stream->SendEos();
            break;
        case HTTP_EVENT_ERROR: {
            auto error = esp_http_client_get_errno(evt->client);
            auto errorMsg = esp_err_to_name(error);
            ESP_LOGE(TAG, "HTTP_EVENT_ERROR, error=%s", errorMsg);
            int mbedtls_err = 0;
            esp_err_t err = esp_tls_get_and_clear_last_error((esp_tls_error_handle_t)evt->data,
                                                             &mbedtls_err, NULL);
            if (err != 0) {
                ESP_LOGE(TAG, "Last esp error code: 0x%x", err);
                ESP_LOGE(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
            }
            // stream->SendError();
            return error;  // Return the actual error code
        } break;
        case HTTP_EVENT_DISCONNECTED: {
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            int mbedtls_err = 0;
            esp_err_t err = esp_tls_get_and_clear_last_error((esp_tls_error_handle_t)evt->data,
                                                             &mbedtls_err, NULL);
            if (err != 0) {
                ESP_LOGW(TAG, "Disconnect with error code: 0x%x, mbedtls: 0x%x", err, mbedtls_err);
                // stream->SendError();
                return err;  // Return error on abnormal disconnect
            }
        } break;
        case HTTP_EVENT_ON_HEADER:
            if (strcmp(evt->header_key, "Content-Length") == 0) {
                stream->content_length_ = atoll(evt->header_value);
                ESP_LOGI(TAG, "Content-Length: %lld", stream->content_length_);
            }
            break;
        // case HTTP_EVENT_ON_HEADER:
        //     ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key,
        //              evt->header_value);
        //     break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGI(TAG, "HTTP_EVENT_REDIRECT");
            esp_http_client_set_redirection(evt->client);
            break;
        default:
            ESP_LOGI(TAG, "http_event_handler, event_id=%d", evt->event_id);
            break;
    }
    return ESP_OK;
}

bool HttpStream::Open(const std::string& url, size_t resume_offset) {
    url_str_ = url;
    download_bytes_received_ = resume_offset;
    resume_offset_ = resume_offset;

    StopRequest();

    ESP_LOGI(TAG, "start OpenTask request => %s", url.c_str());
    xTaskCreate(OpenTask, "OpenTask", 8192, this, 2, &task_handle_);
    return true;
}


void HttpStream::StopRequest() {
    std::lock_guard<std::mutex> lock(mutex_);

    CleanClient();

    if (task_handle_ != nullptr) {
        vTaskDelete(task_handle_);
        task_handle_ = nullptr;
    }
}

void HttpStream::OpenTask(void* arg) {
    auto stream = static_cast<HttpStream*>(arg);
    ESP_LOGI(TAG, "OpenTask, url=%s", stream->url_str_.c_str());
    auto url_str = stream->url_str_.c_str();

    esp_http_client_config_t config = {.url = url_str,
                                       .timeout_ms = 10000,
                                       .max_redirection_count = 5,
                                       .event_handler = http_event_handler,
                                       .user_data = stream,
                                       .crt_bundle_attach = esp_crt_bundle_attach};

    stream->client_ = esp_http_client_init(&config);
    ESP_LOGI(TAG, "OpenTask, client=%p", stream->client_);
    if (stream->client_ == nullptr) {
        return;
    }
    auto client = stream->client_;

    esp_http_client_set_header(client, "Accept",
                               "audio/mpeg, audio/x-mpeg, audio/x-mpeg-3, audio/mpeg3");
    esp_http_client_set_header(client, "Connection", "keep-alive");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "Accept-Language", "zh-CN,zh;q=0.9,en;q=0.8");
    esp_http_client_set_header(client, "User-Agent",
                               "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like "
                               "Gecko) Chrome/148.0.0.0 Safari/537.36");

    // 如果有断点位置，设置 Range 请求头实现断点续传
    if (stream->resume_offset_ > 0) {
        char range_header[64];
        snprintf(range_header, sizeof(range_header), "bytes=%lu-", (unsigned long)stream->resume_offset_);
        esp_http_client_set_header(client, "Range", range_header);
        ESP_LOGI(TAG, "Resuming download with Range header: %s", range_header);
    }



    ESP_LOGI(TAG, "OpenTask, perform request");
    esp_err_t err;
    for (int attempt = 0; attempt < 6; attempt++) {
        stream->content_length_ = 0;
        err = esp_http_client_perform(client);
        if (err == ESP_OK)
            break;
        if (err == ESP_ERR_HTTP_INCOMPLETE_DATA || err == ESP_ERR_HTTP_CONNECTION_CLOSED) {
            ESP_LOGI(TAG, "Redirect retry %d/%d", attempt + 1, 5);
            stream->CleanDataQueue();
            continue;
        }
        break;
    }

    ESP_LOGI(TAG, "OpenTask, perform request err=%d", err);
    if (err == ESP_OK) {
        stream->SendEos();
        ESP_LOGI(TAG, "HTTP GET Status = %d", esp_http_client_get_status_code(client));
    } else {
        stream->SendError();
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }
    stream->task_handle_ = nullptr;
    // 任务自己结束时删除自己，StopRequest 会通过判断 task_handle_ 来决定是否需要删除
    vTaskDelete(nullptr); 
}

void HttpStream::SendError() {
    DataChunk error_chunk;
    error_chunk.status = DataStatus::kError;
    xQueueSend(data_queue_, &error_chunk, portMAX_DELAY);
}
void HttpStream::SendEos() {
    DataChunk eos_chunk;
    eos_chunk.status = DataStatus::kEos;
    xQueueSend(data_queue_, &eos_chunk, portMAX_DELAY);
}

void HttpStream::SendData(DataChunk chunk) {
    TickType_t timeout = pdMS_TO_TICKS(100);
    UBaseType_t queue_count = uxQueueMessagesWaiting(data_queue_);
    if (queue_count >= QUEUE_SIZE) {
        timeout = pdMS_TO_TICKS(5000);
    } else if (queue_count >= CRITICAL_WATER_MARK) {
        timeout = pdMS_TO_TICKS(2000);
    } else if (queue_count >= HIGH_WATER_MARK) {
        timeout = pdMS_TO_TICKS(1000);
    }

    BaseType_t send_result = xQueueSend(data_queue_, &chunk, timeout);
    if (send_result != pdPASS) {
        // 队列仍然满，改为阻塞式等待（不丢包）
        ESP_LOGW(TAG, "Queue full after %dms wait, blocking until space available",
                 timeout / portTICK_PERIOD_MS);
        // 使用 portMAX_DELAY 无限等待，直到队列有空间
        while (xQueueSend(data_queue_, &chunk, timeout) != pdPASS) {
            // 理论上不会到达这里，除非队列被删除
            ESP_LOGW(TAG, "Failed to send chunk even with infinite wait!");
            vTaskDelay(timeout);
        }
    }
}

void HttpStream::CleanDataQueue() {
    DataChunk chunk;
    while (xQueueReceive(data_queue_, &chunk, 0) == pdPASS) {
        if (chunk.data) {
            delete[] chunk.data;
        }
    }
}
QueueHandle_t& HttpStream::GetDataQueue() { return data_queue_; }
int64_t HttpStream::GetContentLength() const { return content_length_; }
int64_t HttpStream::GetDownloadBytesReceived() const { return download_bytes_received_; }

void HttpStream::CleanClient() {
    if (client_ == nullptr) {
        return;
    }
    // esp_http_client_close(client_);
    esp_http_client_cleanup(client_);
    client_ = nullptr;
}