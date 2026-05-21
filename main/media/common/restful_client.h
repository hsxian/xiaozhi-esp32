#pragma once
#include <esp_err.h>
#include <string>

class RestfulClient {
public:
    enum class UserDataType: uint8_t {
        Data,
        Location,
    };
    // 添加一个结构体来保存用户数据
    struct UserDataContext {
        std::string* data;
        UserDataType type;
    };
    RestfulClient(int connect_id = -1);
    ~RestfulClient();
    // 发送GET请求
    std::string Get(const std::string& url);
    // 发送POST请求
    std::string Post(const std::string& url, const std::string& body,
                     const std::string& content_type = "application/json");
    void TryGetRedirectUrl(const std::string& url, std::string& redirect_url,
                           int max_redirects = 5);
    std::string UrlEncode(const std::string& value);
    std::string NormalizeUrl(const std::string& url);

private:
    int connect_id_;
};