#pragma once

#include <string>

struct cJSON;

class JsonHelper {
public:
    bool GetString(const cJSON* item, const char* key, std::string& value) ;
    bool GetNumber(const cJSON* item, const char* key, double& value);
    bool GetNumber(const cJSON* item, const char* key, int& value);
};