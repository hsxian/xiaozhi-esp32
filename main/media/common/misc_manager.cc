#include "misc_manager.h"
#include <format>
#include <string>
#include <vector>
#include "mcp_server.h"
#include "restful_client.h"
#include "wifi_manager.h"

MiscManager& MiscManager::GetInstance() {
    static MiscManager instance;
    return instance;
}

static ReturnValue InternetSearch(const PropertyList& properties) {
    RestfulClient restful_client;
    auto body = std::format("{{\"query\": \"{}\",\"max_results\":{}}}",
                            properties["query"].value<std::string>(),
                            properties["maxResults"].value<int>());
    auto res = restful_client.Post("https://api.anysearch.com/v1/search", body);
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