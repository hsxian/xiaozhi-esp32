#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include "music.h"

class McpTool;
class MusicPlayer;

class MusicManager {
public : 
    static MusicManager& GetInstance();

    // 生成MCP服务器工具
    void GenerateMcpServerTools(std::vector<McpTool*> & tools);

private : 
    MusicManager();
    ~MusicManager() = default;

    void TryResleaseMusicPlayer();

    std::vector<Music*> music_list_;

    MusicPlayer* music_player_ = nullptr;
};