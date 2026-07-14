#pragma once

#include <string>
#include <vector>

struct cJSON;

class JsonHelper {
public:
    bool GetString(const cJSON* item, const char* key, std::string& value) ;
    bool GetNumber(const cJSON* item, const char* key, double& value);
    bool GetNumber(const cJSON* item, const char* key, int& value);
    const cJSON* GetObject(const cJSON* item, const std::vector<const char*>& keys);
    const cJSON* GetArrayItem(const cJSON* item, const std::vector<const char*>& keys,const int index);
};