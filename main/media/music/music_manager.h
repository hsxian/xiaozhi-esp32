#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "music.h"

class McpTool;
class MusicPlayer;

class MusicManager {
public:
    static MusicManager& GetInstance();

    // 生成MCP服务器工具
    void GenerateMcpServerTools(std::vector<McpTool*>& tools);

private:
    MusicManager();
    ~MusicManager() = default;

    void TryResleaseMusicPlayer();

    std::vector<Music*> music_list_;

    std::unique_ptr<MusicPlayer> music_player_;
};