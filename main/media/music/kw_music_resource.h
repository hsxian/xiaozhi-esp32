#pragma once

#include <string>
#include <vector>
#include "../common/query_base.h"
#include "music_resource.h"

class KwMusicResource : public MusicResource {
public:
    virtual bool Search(const QueryBase& query,std::vector<Music>& music_list) override;
    void ParseMusicFromJson(cJSON* item, Music& music);
private:
};