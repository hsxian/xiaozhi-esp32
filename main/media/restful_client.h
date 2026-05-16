#pragma once
#include <string>

class RestfulClient {
public:
    RestfulClient();
    ~RestfulClient();
    // 发送GET请求
    std::string Get(const std::string& url);
    // 发送POST请求
    std::string Post(const std::string& url, const std::string& body, 
                     const std::string& content_type = "application/json");
    void TryGetRedirectUrl(const std::string& url, std::string& redirect_url, int max_redirects = 5);
    std::string UrlEncode(const std::string& value);
    std::string NormalizeUrl(const std::string& url);
};