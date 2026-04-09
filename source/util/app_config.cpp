#include "util/app_config.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <json-c/json.h>
#include <json-c/json_tokener.h>

namespace xxplore::config {

namespace {

struct LanguageMapEntry {
    AppLanguage language;
    const char* id;
    const char* romfsPath;
};

constexpr LanguageMapEntry kLanguageMap[] = {
    {AppLanguage::ZhCn, "zh_cn", "romfs:/i18n/zh_cn.ini"},
    {AppLanguage::ZhTw, "zh_tw", "romfs:/i18n/zh_tw.ini"},
    {AppLanguage::En,   "en",    "romfs:/i18n/en.ini"},
    {AppLanguage::Ja,   "ja",    "romfs:/i18n/ja.ini"},
    {AppLanguage::Ko,   "ko",    "romfs:/i18n/ko.ini"},
    {AppLanguage::Fr,   "fr",    "romfs:/i18n/fr.ini"},
    {AppLanguage::Ru,   "ru",    "romfs:/i18n/ru.ini"},
    {AppLanguage::Es,   "es",    "romfs:/i18n/es.ini"},
};

const LanguageMapEntry* findEntry(AppLanguage language) {
    for (const auto& entry : kLanguageMap)
        if (entry.language == language)
            return &entry;
    return nullptr;
}

std::string errorStringFromErrno() {
    return std::strerror(errno);
}

} // namespace

AppConfig defaultConfig() {
    return {};
}

std::string deriveConfigPath(const char* argv0) {
    if (!argv0 || !argv0[0])
        return {};

    std::string path(argv0);
    std::size_t slash = path.find_last_of("/\\");
    std::size_t dot = path.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return {};

    path.replace(dot, std::string::npos, ".json");
    return path;
}

const char* languageId(AppLanguage language) {
    const auto* entry = findEntry(language);
    return entry ? entry->id : "en";
}

const char* languageRomfsPath(AppLanguage language) {
    const auto* entry = findEntry(language);
    return entry ? entry->romfsPath : "romfs:/i18n/en.ini";
}

bool parseLanguageId(const char* id, AppLanguage& outLanguage) {
    if (!id)
        return false;

    for (const auto& entry : kLanguageMap) {
        if (std::strcmp(entry.id, id) == 0) {
            outLanguage = entry.language;
            return true;
        }
    }
    return false;
}

const char* networkDriveTypeId(NetworkDriveType type) {
    switch (type) {
    case NetworkDriveType::WebDAV: return "webdav";
    case NetworkDriveType::SMB2:   return "smb2";
    }
    return "webdav";
}

bool parseNetworkDriveTypeId(const char* id, NetworkDriveType& out) {
    if (!id) return false;
    if (std::strcmp(id, "webdav") == 0) { out = NetworkDriveType::WebDAV; return true; }
    if (std::strcmp(id, "smb2") == 0)   { out = NetworkDriveType::SMB2;   return true; }
    return false;
}

std::string generateDriveId() {
    // Simple timestamp-based id
    static int counter = 0;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%u_%d",
                  static_cast<unsigned>(std::time(nullptr)), counter++);
    return std::string(buf);
}

bool loadConfigFromArgv0(const char* argv0, AppConfig& outConfig, std::string& outPath) {
    outConfig = defaultConfig();
    outPath = deriveConfigPath(argv0);
    if (outPath.empty()) {
        std::printf("Config: argv0 path unavailable, using defaults\n");
        return false;
    }

    json_object* root = json_object_from_file(outPath.c_str());
    if (!root) {
        std::printf("Config: cannot open %s, using defaults\n", outPath.c_str());
        return false;
    }

    bool ok = false;
    do {
        if (!json_object_is_type(root, json_type_object)) {
            std::printf("Config: invalid root type in %s, using defaults\n", outPath.c_str());
            break;
        }

        json_object* languageObj = nullptr;
        if (!json_object_object_get_ex(root, "language", &languageObj) || !languageObj ||
            !json_object_is_type(languageObj, json_type_string)) {
            std::printf("Config: missing language in %s, using defaults\n", outPath.c_str());
            break;
        }

        AppLanguage language = AppLanguage::En;
        if (!parseLanguageId(json_object_get_string(languageObj), language)) {
            std::printf("Config: invalid language in %s, using defaults\n", outPath.c_str());
            break;
        }

        outConfig.language = language;
        json_object* touchButtonsObj = nullptr;
        if (json_object_object_get_ex(root, "touchButtonsEnabled", &touchButtonsObj) &&
            touchButtonsObj &&
            json_object_is_type(touchButtonsObj, json_type_boolean)) {
            outConfig.touchButtonsEnabled = json_object_get_boolean(touchButtonsObj);
        }
        ok = true;
    } while (false);

    // Parse network drives (optional, does not affect ok flag)
    json_object* drivesArr = nullptr;
    if (json_object_object_get_ex(root, "networkDrives", &drivesArr) && drivesArr &&
        json_object_is_type(drivesArr, json_type_array)) {
        int len = json_object_array_length(drivesArr);
        for (int i = 0; i < len; i++) {
            json_object* drv = json_object_array_get_idx(drivesArr, i);
            if (!drv || !json_object_is_type(drv, json_type_object))
                continue;

            NetworkDriveConfig ndc;
            json_object* val = nullptr;

            if (json_object_object_get_ex(drv, "id", &val) && val)
                ndc.id = json_object_get_string(val);
            if (json_object_object_get_ex(drv, "name", &val) && val)
                ndc.name = json_object_get_string(val);
            if (json_object_object_get_ex(drv, "type", &val) && val)
                parseNetworkDriveTypeId(json_object_get_string(val), ndc.type);
            if (json_object_object_get_ex(drv, "address", &val) && val)
                ndc.address = json_object_get_string(val);
            if (json_object_object_get_ex(drv, "username", &val) && val)
                ndc.username = json_object_get_string(val);
            if (json_object_object_get_ex(drv, "password", &val) && val)
                ndc.password = json_object_get_string(val);

            if (!ndc.id.empty() && !ndc.name.empty())
                outConfig.networkDrives.push_back(std::move(ndc));
        }
    }

    json_object_put(root);
    if (!ok)
        outConfig = defaultConfig();
    return ok;
}

bool saveConfig(const std::string& path, const AppConfig& config, std::string& errOut) {
    if (path.empty()) {
        errOut = "config path unavailable";
        return false;
    }

    json_object* root = json_object_new_object();
    if (!root) {
        errOut = "json allocation failed";
        return false;
    }

    json_object_object_add(root, "language", json_object_new_string(languageId(config.language)));
    json_object_object_add(root, "touchButtonsEnabled",
                           json_object_new_boolean(config.touchButtonsEnabled));

    // Serialize network drives
    json_object* drivesArr = json_object_new_array();
    for (const auto& ndc : config.networkDrives) {
        json_object* drv = json_object_new_object();
        json_object_object_add(drv, "id",       json_object_new_string(ndc.id.c_str()));
        json_object_object_add(drv, "name",     json_object_new_string(ndc.name.c_str()));
        json_object_object_add(drv, "type",     json_object_new_string(networkDriveTypeId(ndc.type)));
        json_object_object_add(drv, "address",  json_object_new_string(ndc.address.c_str()));
        json_object_object_add(drv, "username", json_object_new_string(ndc.username.c_str()));
        json_object_object_add(drv, "password", json_object_new_string(ndc.password.c_str()));
        json_object_array_add(drivesArr, drv);
    }
    json_object_object_add(root, "networkDrives", drivesArr);

    const char* jsonText = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY);
    if (!jsonText) {
        errOut = "json serialization failed";
        json_object_put(root);
        return false;
    }

    FILE* file = std::fopen(path.c_str(), "w");
    if (!file) {
        errOut = errorStringFromErrno();
        json_object_put(root);
        return false;
    }

    std::size_t len = std::strlen(jsonText);
    bool ok = std::fwrite(jsonText, 1, len, file) == len;
    if (ok)
        ok = std::fwrite("\n", 1, 1, file) == 1;

    if (std::fclose(file) != 0 && ok) {
        errOut = errorStringFromErrno();
        ok = false;
    }

    if (!ok && errOut.empty())
        errOut = errorStringFromErrno();

    json_object_put(root);
    return ok;
}

} // namespace xxplore::config
