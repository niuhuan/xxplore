#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace xxplore {

struct InstallQueueItem;

struct InstallBackendCallbacks {
    std::function<void(const std::string&)> onLog;
    std::function<void(const std::string&)> onStatus;
    std::function<void(float currentProgress, float totalProgress)> onProgress;
    std::function<void(uint64_t currentItemBytes, uint64_t currentItemTotal,
                       uint64_t totalBytesDone, uint64_t totalBytesTotal)> onProgressBytes;
    std::function<bool()> shouldAbort;
};

struct InstallSequentialReader {
    std::function<bool(void* outBuffer, size_t size, std::string& errOut)> read;
    std::function<void()> close;

    ~InstallSequentialReader() {
        if (close)
            close();
    }
};

struct InstallDataSourceCallbacks {
    std::function<bool(const InstallQueueItem& item, uint64_t offset, size_t size,
                       void* outBuffer, std::string& errOut)> readRange;
    std::function<std::unique_ptr<InstallSequentialReader>(const InstallQueueItem& item,
                                                           uint64_t offset,
                                                           uint64_t expectedSize,
                                                           std::string& errOut)>
        openSequentialRead;
};

bool runInstallQueue(const std::vector<InstallQueueItem>& items, bool installToNand,
                     bool deleteAfterInstall, const InstallBackendCallbacks& callbacks,
                     std::string& errOut,
                     const InstallDataSourceCallbacks* sourceCallbacks = nullptr);

} // namespace xxplore
