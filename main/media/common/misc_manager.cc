#include "misc_manager.h"
#include "mcp_server.h"
#include "wifi_manager.h"

MiscManager& MiscManager::GetInstance() {
    static MiscManager instance;
    return instance;
}

void MiscManager::GenerateMcpServerTools(std::vector<McpTool*>& tools) {
    // 获取局域网IP地址
    auto tool = new McpTool("self.misc.local_ip_address", "a tool to get local ip address.",
                            PropertyList(), [](const PropertyList& properties) -> ReturnValue {
                                auto& wifi = WifiManager::GetInstance();
                                return wifi.GetIpAddress();
                            });
    tools.push_back(tool);
}
