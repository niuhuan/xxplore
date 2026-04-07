#pragma once
#include <string>
#include <vector>

namespace inst::config {

inline const std::string appDir = "sdmc:/switch/Xplore";
inline const std::string configPath = appDir + "/config.json";
inline const std::string appVersion = "0.1.0";

extern std::string gAuthKey;
extern std::string sigPatchesUrl;
extern std::string lastNetUrl;
extern std::vector<std::string> updateInfo;
extern int languageSetting;
extern bool ignoreReqVers;
extern bool validateNCAs;
extern bool overClock;
extern bool deletePrompt;
extern bool autoUpdate;
extern bool gayMode;
extern bool usbAck;

} // namespace inst::config
