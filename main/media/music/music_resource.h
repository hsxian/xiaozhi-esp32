#pragma once

#include <string>
#include "music.h"
#include "../common/query_base.h"
#include <vector>

class MusicResource {
public:
    virtual bool Search(const QueryBase& query,std::vector<Music>& music_list) = 0;
private:
};