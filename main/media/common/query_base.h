#pragma once

#include <string>

struct QueryBase {
public:
    std::string keyword;
    int page = 1;
    int page_size = 10;
};