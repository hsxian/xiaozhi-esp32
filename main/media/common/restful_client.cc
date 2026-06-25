#include "restful_client.h"
#include <esp_log.h>
#include <iomanip>
#include <sstream>
#include "board.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_tls.h"

#define TAG "RestfulClient"

#define MAX_HTTP_TIMEOUT_MS 60000
RestfulClient::RestfulClient() {}

RestfulClient::~RestfulClient() {}

esp_err_t _http_event_handler(esp_http_client_event_t* evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA: {
            auto data_ctx = static_cast<RestfulClient::UserDataContext*>(evt->user_data);
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (data_ctx->type == RestfulClient::UserDataType::Data) {
                data_ctx->data->append((const char*)(evt->data), evt->data_len);
            }

        } break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED: {
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            int mbedtls_err = 0;
            esp_err_t err = esp_tls_get_and_clear_last_error((esp_tls_error_handle_t)evt->data,
                                                             &mbedtls_err, NULL);
            if (err != 0) {
                ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
                ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
            }
        } break;
        // case HTTP_EVENT_ON_HEADER: {
        //     ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key,
        //              evt->header_value);
        //     if (strcmp(evt->header_key, "Location") == 0) {
        //         ESP_LOGI(TAG, "Redirect Location: %s", evt->header_value);
        //         auto data_ctx = static_cast<RestfulClient::UserDataContext*>(evt->user_data);
        //         if (data_ctx->type == RestfulClient::UserDataType::Location) {
        //             data_ctx->data->assign(evt->header_value);
        //         }
        //     }
        // } break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGI(TAG, "HTTP_EVENT_REDIRECT");
            esp_http_client_set_redirection(evt->client);
            break;
        case HTTP_EVENT_ERROR: {
            ESP_LOGI(TAG, "HTTP_EVENT_ERROR");

            int mbedtls_err = 0;
            esp_err_t err = esp_tls_get_and_clear_last_error((esp_tls_error_handle_t)evt->data,
                                                             &mbedtls_err, NULL);
            if (err != 0) {
                ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
                ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
            }
        } break;
        default:
            ESP_LOGD(TAG, "HTTP_EVENT %d", evt->event_id);
            break;
    }
    return ESP_OK;
}
esp_http_client_handle_t RestfulClient::CreateClient(const char* url, UserDataContext& dc, const std::map<std::string, std::string>& headers) {
    esp_http_client_config_t config = {
        .url = url,  // 可换成你的 API URL
        // .tls_version = ESP_HTTP_CLIENT_TLS_VER_TLS_1_2,
        .timeout_ms = MAX_HTTP_TIMEOUT_MS,
        .max_redirection_count = 5,  // 最大重定向次数
        .event_handler = _http_event_handler,
        .user_data = &dc,
        // .use_global_ca_store = true,                 // 使用全局CA存储
        // .skip_cert_common_name_check = true,         // 跳过CN检查
        .crt_bundle_attach = esp_crt_bundle_attach,  // 启用内置根证书
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    // esp_http_client_set_header(client, "User-Agent", USER_AGENT);
    esp_http_client_set_header(client, "Accept",
                               "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
    esp_http_client_set_header(client, "Accept-Language", "en-US,en;q=0.9");
    // esp_http_client_set_header(client, "Accept-Encoding", "gzip, deflate");
    esp_http_client_set_header(client, "Cache-Control", "no-cache");
    esp_http_client_set_header(client, "DNT", "1");
    for (const auto& header : headers) {
        esp_http_client_set_header(client, header.first.c_str(), header.second.c_str());
    }
    return client;
}

esp_err_t RestfulClient::PerformLoop(esp_http_client_handle_t client, UserDataContext& dc,
                                     const char* log_tip) {
    esp_err_t err;
    for (int attempt = 0; attempt < 6; attempt++) {
        if (attempt > 0) {
            char url_buf[256];
            esp_http_client_get_url(client, url_buf, sizeof(url_buf) - 1);
            ESP_LOGI(TAG, "%s retry %d, URL: %s", log_tip, attempt, url_buf);
        }
        err = esp_http_client_perform(client);
        if (err == ESP_OK)
            break;
        ESP_LOGI(TAG, "%s perform result: %s", log_tip, esp_err_to_name(err));
        // 301/302 响应 body 使用 chunked 编码时，连接关闭会导致
        // ESP_ERR_HTTP_INCOMPLETE_DATA/CONNECTION_CLOSED。
        // 此时内部重定向已设置 process_again=1，重试即可跟随重定向。
        if (err == ESP_ERR_HTTP_INCOMPLETE_DATA || err == ESP_ERR_HTTP_CONNECTION_CLOSED) {
            ESP_LOGI(TAG, "%s redirect retry %d/%d", log_tip, attempt + 1, 5);
            dc.data->clear();
            continue;
        }
        break;
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s status = %d", log_tip, esp_http_client_get_status_code(client));
    } else {
        ESP_LOGE(TAG, "%s request failed: %s", log_tip, esp_err_to_name(err));
        dc.data->clear();
    }
    ESP_LOGI(TAG, "%s response => %d bytes", log_tip, dc.data->size());
    return err;
}
std::string RestfulClient::Get(const std::string& url) {
    return Get(url, {});
}

std::string RestfulClient::Get(const std::string& url, const std::map<std::string, std::string>& headers) {
    auto url_str = url.c_str();
    std::string response;
    UserDataContext dc = {&response, UserDataType::Data};
    esp_http_client_handle_t client = CreateClient(url_str, dc, headers);

    ESP_LOGI(TAG, "HTTP GET request => %s", url_str);
    // GET 请求
    PerformLoop(client, dc, "HTTP GET");

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    dc.data = nullptr;
    return response;
}

std::string RestfulClient::Post(const std::string& url, const std::string& body,
                                const std::string& content_type) {
    return Post(url, body, {{"Content-Type", content_type}});
}

std::string RestfulClient::Post(const std::string& url, const std::string& body,
                                const std::map<std::string, std::string>& headers) {
    auto url_str = url.c_str();
    std::string response;
    UserDataContext dc = {&response, UserDataType::Data};
    esp_http_client_handle_t client = CreateClient(url_str, dc, headers);

    ESP_LOGI(TAG, "HTTP POST request => %s", url_str);
    esp_http_client_set_post_field(client, body.c_str(), body.length());
    PerformLoop(client, dc, "HTTP POST");

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    dc.data = nullptr;
    return response;
}

std::string RestfulClient::NormalizeUrl(const std::string& url) {
    const std::string scheme = "://";
    auto scheme_pos = url.find(scheme);
    if (scheme_pos == std::string::npos) {
        return url;
    }

    auto host_start = scheme_pos + scheme.size();
    auto host_end = url.find_first_of("/?#", host_start);
    if (host_end == std::string::npos) {
        return url + "/";
    }

    if (url[host_end] == '/') {
        return url;
    }

    return url.substr(0, host_end) + "/" + url.substr(host_end);
}

std::string RestfulClient::UrlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex << std::uppercase;

    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::setw(2) << int(c);
        }
    }
    return escaped.str();
}