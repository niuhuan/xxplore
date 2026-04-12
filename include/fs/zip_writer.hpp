#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace xxplore::fs {

class ProviderManager;

struct ZipWriteSource {
    std::string fullPath;
    std::string archiveRootName;
    bool        isDirectory = false;
};

struct ZipWriteProgress {
    std::string currentPath;
    uint64_t    currentFileBytes = 0;
    uint64_t    currentFileTotalBytes = 0;
    uint64_t    overallBytes = 0;
    uint64_t    overallTotalBytes = 0;
};

using ZipWriteProgressCb = std::function<bool(const ZipWriteProgress&)>;

bool createZipArchive(ProviderManager& provMgr, const std::string& destFullPath,
                      const std::vector<ZipWriteSource>& sources, std::string& errOut,
                      const ZipWriteProgressCb& progressCb = nullptr);

} // namespace xxplore::fs
