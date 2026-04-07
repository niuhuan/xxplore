#include "util/app_config.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <json-c/json.h>
#include <json-c/json_tokener.h>

namespace xplore::config {

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
        ok = true;
    } while (false);

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

} // namespace xplore::config
