#pragma once
#include <string>
#include <vector>

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

enum class NetworkDriveType {
    WebDAV,
    SMB2,
};

struct NetworkDriveConfig {
    std::string id;        // unique id (auto-generated)
    std::string name;      // display name
    NetworkDriveType type = NetworkDriveType::WebDAV;
    std::string address;   // e.g. "http://192.168.1.1/dav" or "192.168.1.1/share"
    std::string username;
    std::string password;
};

struct AppConfig {
    AppLanguage language = AppLanguage::En;
    bool touchButtonsEnabled = false;
    std::vector<NetworkDriveConfig> networkDrives;
};

AppConfig defaultConfig();
std::string deriveConfigPath(const char* argv0);
bool loadConfigFromArgv0(const char* argv0, AppConfig& outConfig, std::string& outPath);
bool saveConfig(const std::string& path, const AppConfig& config, std::string& errOut);
const char* languageId(AppLanguage language);
const char* languageRomfsPath(AppLanguage language);
bool parseLanguageId(const char* id, AppLanguage& outLanguage);
const char* networkDriveTypeId(NetworkDriveType type);
bool parseNetworkDriveTypeId(const char* id, NetworkDriveType& out);
std::string generateDriveId();

} // namespace xplore::config
