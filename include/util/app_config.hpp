#pragma once
#include <string>

namespace xplore::config {

enum class AppLanguage {
    ZhCn,
    ZhTw,
    En,
    Ja,
    Ko,
    Fr,
    Ru,
    Es,
};

struct AppConfig {
    AppLanguage language = AppLanguage::En;
};

AppConfig defaultConfig();
std::string deriveConfigPath(const char* argv0);
bool loadConfigFromArgv0(const char* argv0, AppConfig& outConfig, std::string& outPath);
bool saveConfig(const std::string& path, const AppConfig& config, std::string& errOut);
const char* languageId(AppLanguage language);
const char* languageRomfsPath(AppLanguage language);
bool parseLanguageId(const char* id, AppLanguage& outLanguage);

} // namespace xplore::config
