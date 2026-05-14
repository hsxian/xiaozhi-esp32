#include "restful_client.h"
#include "board.h"
#include <esp_log.h>

#define TAG "RestfulClient"

RestfulClient::RestfulClient()
{
}

RestfulClient::~RestfulClient()
{
}

std::string RestfulClient::Get(const std::string& url)
{
    std::string response;
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection for: %s", url.c_str());
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
                                const std::string& content_type)
{
    std::string response;
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection for: %s", url.c_str());
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