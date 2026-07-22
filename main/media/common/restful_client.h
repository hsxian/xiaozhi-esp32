#pragma once
#include <esp_err.h>
#include <map>
#include <string>
typedef struct esp_http_client* esp_http_client_handle_t;
class RestfulClient {
public:
    enum class UserDataType : uint8_t {
        Data,
        Location,
    };
    // 添加一个结构体来保存用户数据
    struct UserDataContext {
        std::string* data;
        UserDataType type;
    };
    RestfulClient();
    ~RestfulClient();
    // 发送GET请求
    std::string Get(const std::string& url);
    std::string Get(const std::string& url, const std::map<std::string, std::string>& headers);
    // 发送POST请求
    std::string Post(const std::string& url, const std::string& body,
                     const std::string& content_type = "application/json");
    std::string Post(const std::string& url, const std::string& body,
                     const std::map<std::string, std::string>& headers);
    std::string UrlEncode(const std::string& value);
    std::string NormalizeUrl(const std::string& url);

private:
    esp_http_client_handle_t CreateClient(const char* url, UserDataContext& dc,
                                          const std::map<std::string, std::string>& headers = {});
    esp_err_t PerformLoop(esp_http_client_handle_t client, UserDataContext& dc,
                          const char* log_tip);
};