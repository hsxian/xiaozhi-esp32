#include "json_helper.h"
#include "cJSON.h"

bool JsonHelper::GetString(const cJSON* item, const char* key, std::string& value) {
    auto sub_item = cJSON_GetObjectItem(item, key);
    if (sub_item && cJSON_IsString(sub_item)) {
        value = cJSON_GetStringValue(sub_item);
        return true;
    }
    return false;
};
bool JsonHelper::GetNumber(const cJSON* item, const char* key, double& value) {
    auto sub_item = cJSON_GetObjectItem(item, key);
    if (sub_item && cJSON_IsNumber(sub_item)) {
        value = cJSON_GetNumberValue(sub_item);
        return true;
    }
    return false;
};

bool JsonHelper::GetNumber(const cJSON* item, const char* key, int& value) {
    auto sub_item = cJSON_GetObjectItem(item, key);
    if (sub_item && cJSON_IsNumber(sub_item)) {
        value = static_cast<int>(cJSON_GetNumberValue(sub_item));
        return true;
    }
    return false;
};

const cJSON* JsonHelper::GetObject(const cJSON* item, const std::vector<const char*>& keys) {
    const cJSON* current = item;
    for (const auto& key : keys) {
        if (!current)
            return nullptr;
        current = cJSON_GetObjectItem(current, key);
    }
    return current;
};

const cJSON* JsonHelper::GetArrayItem(const cJSON* item, const std::vector<const char*>& keys,
                                      const int index) {
    const cJSON* array = GetObject(item, keys);
    if (array && cJSON_IsArray(array)) {
        return cJSON_GetArrayItem(array, index);
    }
    return nullptr;
};