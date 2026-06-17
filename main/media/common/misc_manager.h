#pragma once

#include <vector>

class McpTool;

class MiscManager {
public:
    static MiscManager& GetInstance();

    MiscManager(const MiscManager&) = delete;
    MiscManager& operator=(const MiscManager&) = delete;
    // MCP tools
    void GenerateMcpServerTools(std::vector<McpTool*>& tools);

private:
    MiscManager()=default;
    ~MiscManager()=default;
};
