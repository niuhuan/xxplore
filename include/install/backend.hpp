#pragma once
#include <functional>
#include <string>
#include <vector>

namespace xplore {

struct InstallQueueItem;

struct InstallBackendCallbacks {
    std::function<void(const std::string&)> onLog;
    std::function<void(const std::string&)> onStatus;
    std::function<void(float currentProgress, float totalProgress)> onProgress;
};

bool runInstallQueue(const std::vector<InstallQueueItem>& items, bool installToNand,
                     bool deleteAfterInstall, const InstallBackendCallbacks& callbacks,
                     std::string& errOut);

} // namespace xplore
