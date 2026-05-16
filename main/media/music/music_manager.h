#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include "music.h"

class McpTool;
class Mp3MusicPlayer;

class MusicManager {
public:
    static MusicManager& GetInstance();

    // 生成MCP服务器工具
    void GenerateMcpServerTools(std::vector<McpTool*>& tools);

private:
    MusicManager();
    ~MusicManager() = default;

    std::vector<Music> music_search_list_;
    std::vector<Music> music_list_;

    Mp3MusicPlayer* mp3_player_ = nullptr;
};