#pragma once
#include <cstddef>
#include <cstdint>
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

struct InstallDataSourceCallbacks {
    std::function<bool(const InstallQueueItem& item, uint64_t offset, size_t size,
                       void* outBuffer, std::string& errOut)> readRange;
};

bool runInstallQueue(const std::vector<InstallQueueItem>& items, bool installToNand,
                     bool deleteAfterInstall, const InstallBackendCallbacks& callbacks,
                     std::string& errOut,
                     const InstallDataSourceCallbacks* sourceCallbacks = nullptr);

} // namespace xplore
