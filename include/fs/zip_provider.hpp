#pragma once
#include "fs/file_provider.hpp"
#include <memory>

namespace xxplore {
namespace fs {

class ProviderManager;

std::shared_ptr<FileProvider> createZipProvider(ProviderManager* provMgr,
                                                const std::string& archiveFullPath,
                                                bool appletMode,
                                                std::string& errOut);

} // namespace fs
} // namespace xxplore
