#pragma once

#include <string>
#include <vector>

struct LyricsLine {
    int time_ms;          // 时间戳（毫秒）
    std::string text;     // 歌词文本
};

class Lyrics {
private:
    std::vector<LyricsLine> lines_;   // 歌词行列表
    int current_line_index_;           // 当前播放到的歌词索引

public:
    Lyrics();
    
    // 解析LRC格式的歌词字符串
    bool Parse(const std::string& lrc_content);
    
    // 根据当前播放时间（毫秒）获取对应的歌词
    bool GetLyricAtTime(int current_time_ms, std::string& out_lyric);
    
    // 获取当前歌词索引
    int GetCurrentLineIndex() const;
    
    // 获取总行数
    int GetLineCount() const;
    
    // 获取指定索引的歌词
    bool GetLineAt(int index, LyricsLine& out_line);
    
    // 清空歌词
    void Clear();
    
    // 检查是否有歌词
    bool HasLyrics() const;
};
