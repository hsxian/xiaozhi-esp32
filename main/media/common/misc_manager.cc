#include "misc_manager.h"
#include <cstring>
#include <format>
#include <string>
#include <vector>
#include "mcp_server.h"
#include "restful_client.h"
#include "wifi_manager.h"

#include <esp_log.h>

#define TAG "MiscManager"

MiscManager& MiscManager::GetInstance() {
    static MiscManager instance;
    return instance;
}

static ReturnValue InternetSearch(const PropertyList& properties) {
    RestfulClient restful_client;
    auto max_results = properties["maxResults"].value<int>();
    auto body = std::format("{{\"query\": \"{}\",\"max_results\":{}}}",
                            properties["query"].value<std::string>(), max_results);
    ESP_LOGI(TAG, "Search request body: %s", body.c_str());
    auto res = restful_client.Post("https://api.anysearch.com/v1/search", body);

    constexpr size_t kMaxResponseSize = 1204 * 6;  // 1204*7 bytes

    if (res.size() > kMaxResponseSize) {
        ESP_LOGW(TAG, "Search response too large (%zu bytes), truncating", res.size());
        cJSON* root = cJSON_Parse(res.c_str());
        if (root != nullptr) {
            cJSON* data = cJSON_GetObjectItem(root, "data");
            cJSON* results = nullptr;
            if (cJSON_IsObject(data)) {
                results = cJSON_GetObjectItem(data, "results");
            }
            if (cJSON_IsArray(results)) {
                int count = cJSON_GetArraySize(results);
                // 预留 JSON 结构开销（键名、括号、逗号等），每条结果约 64 字节
                size_t json_overhead = count * 64 + 8;
                size_t remaining_budget =
                    (kMaxResponseSize > json_overhead) ? kMaxResponseSize - json_overhead : 0;

                for (int i = 0; i < count; i++) {
                    cJSON* result = cJSON_GetArrayItem(results, i);
                    if (cJSON_IsObject(result)) {
                        // snippet、url、title 对 AI 智能体有用，保留并计入预算
                        for (const char* field : {"snippet", "url", "title"}) {
                            cJSON* item = cJSON_GetObjectItem(result, field);
                            if (cJSON_IsString(item) && item->valuestring != nullptr) {
                                size_t len = strlen(item->valuestring);
                                remaining_budget =
                                    (remaining_budget > len) ? remaining_budget - len : 0;
                            }
                        }
                        // 截断 content 字段
                        cJSON* content = cJSON_GetObjectItem(result, "content");
                        if (cJSON_IsString(content) && content->valuestring != nullptr) {
                            if (remaining_budget == 0) {
                                content->valuestring[0] = '\0';
                            } else {
                                int remaining = count - i;
                                size_t max_content_length = remaining_budget / remaining;
                                if (strlen(content->valuestring) > max_content_length) {
                                    content->valuestring[max_content_length] = '\0';
                                }
                                size_t used = strlen(content->valuestring);
                                remaining_budget =
                                    (remaining_budget > used) ? remaining_budget - used : 0;
                            }
                        }
                        // 预算耗尽，删除后续所有结果
                        if (remaining_budget == 0 && i + 1 < count) {
                            for (int j = count - 1; j > i; j--) {
                                cJSON_DeleteItemFromArray(results, j);
                            }
                            break;
                        }
                    }
                }
            }
            char* json_str = cJSON_PrintUnformatted(results);
            if (json_str != nullptr) {
                res = std::string(json_str);
                cJSON_free(json_str);
            }
            cJSON_Delete(root);
        } else {
            res.resize(kMaxResponseSize);
            ESP_LOGW(TAG, "Failed to parse search response JSON, raw truncated");
        }
    }
    ESP_LOGI(TAG, "Search response: %d bytes", res.size());
    return res;
}

void MiscManager::GenerateMcpServerTools(std::vector<McpTool*>& tools) {
    // 获取局域网IP地址
    auto tool = new McpTool("self.misc.local_ip_address", "a tool to get local ip address.",
                            PropertyList(), [](const PropertyList& properties) -> ReturnValue {
                                auto& wifi = WifiManager::GetInstance();
                                return wifi.GetIpAddress();
                            });
    tools.push_back(tool);

    // 提供联网查询功能，用于查询互联网知识，比如：百科知识，新闻知识等
    tool = new McpTool("self.misc.internet_search", "a tool to search internet.",
                       PropertyList({Property("query", kPropertyTypeString, ""),
                                     Property("maxResults", kPropertyTypeInteger, 3, 1, 10)}),
                       InternetSearch);
    tools.push_back(tool);
}