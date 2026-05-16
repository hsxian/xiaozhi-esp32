#include "restful_client.h"
#include <esp_log.h>
#include <iomanip>
#include <sstream>
#include "board.h"

#define TAG "RestfulClient"
#define USER_AGENT                                                                             \
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/148.0.0.0 " \
    "Safari/537.36"
RestfulClient::RestfulClient() {}

RestfulClient::~RestfulClient() {}

std::string RestfulClient::Get(const std::string& url) {
    std::string response;
    auto http = Board::GetInstance().GetNetwork()->CreateHttp();
    http->SetHeader("User-Agent", USER_AGENT);
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Get Failed to open HTTP connection for: %s", url.c_str());
        return response;
    }

    int status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP request failed, status code: %d", status_code);
        http->Close();
        return response;
    }

    response = http->ReadAll();
    http->Close();
    ESP_LOGI(TAG, "HTTP response received: %d bytes", response.length());
    return response;
}

std::string RestfulClient::Post(const std::string& url, const std::string& body,
                                const std::string& content_type) {
    std::string response;
    auto http = Board::GetInstance().GetNetwork()->CreateHttp();
    http->SetHeader("User-Agent", USER_AGENT);
    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Post Failed to open HTTP connection for: %s", url.c_str());
        return response;
    }

    // 设置Content-Type请求头
    http->SetHeader("Content-Type", content_type);

    // 写入请求体
    http->Write(body.c_str(), body.size());
    http->Write("", 0);  // 结束写入

    int status_code = http->GetStatusCode();
    if (status_code != 200 && status_code != 201) {
        ESP_LOGE(TAG, "HTTP POST request failed, status code: %d", status_code);
        http->Close();
        return response;
    }

    response = http->ReadAll();
    http->Close();
    ESP_LOGI(TAG, "HTTP POST response received: %d bytes", response.length());
    return response;
}

void RestfulClient::TryGetRedirectUrl(const std::string& url, std::string& redirect_url, int max_redirects) {
    if (max_redirects <= 0) {
        ESP_LOGE(TAG, "Max redirects reached for URL: %s", url.c_str());
        return;
    }   
    auto http = Board::GetInstance().GetNetwork()->CreateHttp();
    http->SetHeader("User-Agent", USER_AGENT);
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "TryGetRedirectUrl Failed to open HTTP connection for: %s", url.c_str());
        return;
    }

    int status_code = http->GetStatusCode();
    if (status_code == 301 || status_code == 302 || status_code == 307 || status_code == 308) {
        redirect_url = http->GetResponseHeader("Location");
        ESP_LOGI(TAG, "Redirect URL: %s", redirect_url.c_str());   
        auto normalized_url = NormalizeUrl(redirect_url);
        TryGetRedirectUrl(normalized_url, redirect_url, max_redirects - 1);
    }
    http->Close();
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