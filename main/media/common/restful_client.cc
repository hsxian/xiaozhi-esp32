#include "restful_client.h"
#include <esp_log.h>
#include <iomanip>
#include <sstream>
#include "board.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_tls.h"

#define TAG "RestfulClient"
#define USER_AGENT                                                                             \
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/148.0.0.0 " \
    "Safari/537.36"

#define MAX_HTTP_TIMEOUT_MS 60000
RestfulClient::RestfulClient(int connect_id) : connect_id_(connect_id) {

}

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
        case HTTP_EVENT_ON_HEADER:
        {
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            if (strcmp(evt->header_key, "Location") == 0) {
                auto data_ctx = static_cast<RestfulClient::UserDataContext*>(evt->user_data);
                if (data_ctx->type == RestfulClient::UserDataType::Location) {
                    data_ctx->data->assign(evt->header_value);
                }
            }
        } break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGI(TAG, "HTTP_EVENT_REDIRECT");
            esp_http_client_set_redirection(evt->client);
            break;
        default:
            ESP_LOGI(TAG, "HTTP_EVENT %d", evt->event_id);
            break;
    }
    return ESP_OK;
}
std::string RestfulClient::Get(const std::string& url) {
    auto url_str = url.c_str();

    std::string response;
    UserDataContext response_ctx = {&response, UserDataType::Data};
    esp_http_client_config_t config = {
        .url = url_str,  // 可换成你的 API
        .timeout_ms = MAX_HTTP_TIMEOUT_MS,
        .max_redirection_count = 5,  // 最大重定向次数
        .event_handler = _http_event_handler,
        .user_data = &response_ctx,
        // .use_global_ca_store = true,                 // 使用全局CA存储
        // .skip_cert_common_name_check = true,         // 跳过CN检查
        .crt_bundle_attach = esp_crt_bundle_attach,  // 启用内置根证书
    };
    ESP_LOGI(TAG, "HTTP native request => %s", url_str);

    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_http_client_set_header(client, "User-Agent", USER_AGENT);
    esp_http_client_set_header(client, "Accept", "*/*");



    // GET
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP GET Status = %d", esp_http_client_get_status_code(client));
    } else {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "HTTP GET response => %d", response.length());

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return response;
}

std::string RestfulClient::Post(const std::string& url, const std::string& body,
                                const std::string& content_type) {
    std::string response;
    const int max_retries = 2;

    for (int retry = 0; retry <= max_retries; retry++) {
        auto http = Board::GetInstance().GetNetwork()->CreateHttp(connect_id_);
        http->SetHeader("User-Agent", USER_AGENT);

        if (!http->Open("POST", url)) {
            ESP_LOGE(TAG, "Post Failed to open HTTP connection for: %s (retry %d/%d)", url.c_str(),
                     retry, max_retries);
            continue;
        }

        // 设置Content-Type请求头
        http->SetHeader("Content-Type", content_type);

        // 写入请求体
        http->Write(body.c_str(), body.size());
        http->Write("", 0);  // 结束写入

        int status_code = http->GetStatusCode();
        if (status_code != 200 && status_code != 201) {
            ESP_LOGE(TAG, "HTTP POST request failed, status code: %d (retry %d/%d)", status_code,
                     retry, max_retries);
            http->Close();
            continue;
        }

        response = http->ReadAll();

        // Check if response was received (connection may have closed prematurely)
        if (response.empty()) {
            ESP_LOGE(
                TAG,
                "Empty response received, connection may have closed prematurely (retry %d/%d)",
                retry, max_retries);
            http->Close();
            continue;
        }

        http->Close();
        ESP_LOGI(TAG, "HTTP POST response received: %d bytes", response.length());
        return response;
    }

    ESP_LOGE(TAG, "All %d retries failed for URL: %s", max_retries + 1, url.c_str());
    return response;
}

void RestfulClient::TryGetRedirectUrl(const std::string& url, std::string& redirect_url, int max_redirects) {
    if (max_redirects <= 0) {
        ESP_LOGE(TAG, "Max redirects reached for URL: %s", url.c_str());
        return;
    }
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(connect_id_);
    http->SetHeader("User-Agent", USER_AGENT);
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "TryGetRedirectUrl Failed to open HTTP connection for: %s", url.c_str());
        return;
    }

    int status_code = http->GetStatusCode();
    if (status_code == 301 || status_code == 302 || status_code == 307 || status_code == 308) {
        redirect_url = http->GetResponseHeader("Location");
        http->Close();
        ESP_LOGD(TAG, "Redirect URL: %s", redirect_url.c_str());
        auto normalized_url = NormalizeUrl(redirect_url);
        TryGetRedirectUrl(normalized_url, redirect_url, max_redirects - 1);
    } else {
        http->Close();
    }
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