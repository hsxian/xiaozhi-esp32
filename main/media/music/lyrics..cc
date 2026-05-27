#include "lyrics.h"
#include <algorithm>
#include "esp_log.h"

#define TAG "Lyrics"

Lyrics::Lyrics() : current_line_index_(-1) {}

bool Lyrics::Parse(const std::string& lrc_content) {
    Clear();
    
    if (lrc_content.empty()) {
        ESP_LOGW(TAG, "Empty lyrics content");
        return false;
    }

    const char* content = lrc_content.c_str();
    const char* pos = content;
    
    while (*pos != '\0') {
        // 查找时间标签 [mm:ss.xx]
        if (*pos == '[') {
            pos++;
            
            // 解析时间格式 [mm:ss.xx]
            int minutes = 0, seconds = 0, milliseconds = 0;
            int result = sscanf(pos, "%d:%d.%d", &minutes, &seconds, &milliseconds);
            
            if (result >= 2) {
                // 计算总毫秒数
                int time_ms = minutes * 60 * 1000 + seconds * 1000 + milliseconds;
                
                // 跳过时间标签
                while (*pos != '\0' && *pos != ']') {
                    pos++;
                }
                
                if (*pos == ']') {
                    pos++;
                    
                    // 获取歌词文本（直到换行或下一个时间标签）
                    const char* text_start = pos;
                    while (*pos != '\0' && *pos != '[' && *pos != '\n' && *pos != '\r') {
                        pos++;
                    }
                    
                    std::string text(text_start, pos);
                    
                    // 去除首尾空白
                    size_t start = text.find_first_not_of(" \t");
                    size_t end = text.find_last_not_of(" \t");
                    if (start != std::string::npos && end != std::string::npos) {
                        text = text.substr(start, end - start + 1);
                    } else {
                        text = "";
                    }
                    
                    // 如果歌词文本不为空，添加到列表
                    if (!text.empty()) {
                        LyricsLine line;
                        line.time_ms = time_ms;
                        line.text = text;
                        // ESP_LOGI(TAG, "Parsed line: %s, time: %d", line.text.c_str(), line.time_ms);
                        lines_.push_back(line);
                    }
                }
            } else {
                // 解析失败，跳过这个标签
                while (*pos != '\0' && *pos != ']') {
                    pos++;
                }
                if (*pos == ']') {
                    pos++;
                }
            }
        } else {
            pos++;
        }
    }
    
    // 按时间戳排序
    std::sort(lines_.begin(), lines_.end(), [](const LyricsLine& a, const LyricsLine& b) {
        return a.time_ms < b.time_ms;
    });
    
    ESP_LOGI(TAG, "Parsed %d lyrics lines", lines_.size());
    return !lines_.empty();
}

bool Lyrics::GetLyricAtTime(int current_time_ms, std::string& out_lyric) {
    out_lyric.clear();
    
    if (lines_.empty()) {
        return false;
    }
    
    // 找到当前时间对应的歌词行
    int line_count = lines_.size();
    int target_index = -1;
    
    for (int i = 0; i < line_count; i++) {
        if (lines_[i].time_ms <= current_time_ms) {
            target_index = i;
        } else {
            break;
        }
    }
    
    if (target_index >= 0 && target_index < line_count) {
        out_lyric = lines_[target_index].text;
        current_line_index_ = target_index;
        // ESP_LOGI(TAG, "Current time: %d, target index: %d", current_time_ms, target_index);
        return true;
    }
    
    return false;
}

int Lyrics::GetCurrentLineIndex() const {
    return current_line_index_;
}

int Lyrics::GetLineCount() const {
    return lines_.size();
}

bool Lyrics::GetLineAt(int index, LyricsLine& out_line) {
    if (index < 0 || index >= static_cast<int>(lines_.size())) {
        return false;
    }
    out_line = lines_[index];
    return true;
}

void Lyrics::Clear() {
    lines_.clear();
    current_line_index_ = -1;
}

bool Lyrics::HasLyrics() const {
    return !lines_.empty();
}
