#pragma once
#include "fs/file_provider.hpp"

namespace xplore {
namespace fs {

/// Local filesystem provider wrapping POSIX/sdmc operations.
class LocalFileProvider : public FileProvider {
public:
    std::string providerId() const override { return "local"; }
    std::string displayPrefix() const override { return "sdmc:"; }
    bool isReadOnly() const override { return false; }
    bool allowsSelection() const override { return true; }

    std::vector<FileEntry> listDir(const std::string& path,
                                   std::string& errOut) override;
    bool statPath(const std::string& path, FileStatInfo& out,
                  std::string& errOut) override;
    bool createDirectory(const std::string& path, std::string& errOut) override;
    bool removeAll(const std::string& path, std::string& errOut) override;
    bool renamePath(const std::string& from, const std::string& to,
                    std::string& errOut) override;
    bool readFile(const std::string& path, uint64_t offset, size_t size,
                  void* outBuffer, std::string& errOut) override;
    bool writeFile(const std::string& path, const void* data, size_t size,
                   std::string& errOut) override;

    /// Override: uses POSIX copy (chunked, supports large files).
    bool copyFile(const std::string& src, const std::string& dst,
                  std::string& errOut, const ProviderProgressCb& cb) override;
    bool moveFile(const std::string& src, const std::string& dst,
                  std::string& errOut, const ProviderProgressCb& cb) override;
    bool copyEntry(const std::string& src, const std::string& dst,
                   std::string& errOut, const ProviderProgressCb& cb) override;
    bool moveEntry(const std::string& src, const std::string& dst,
                   std::string& errOut, const ProviderProgressCb& cb) override;
};

} // namespace fs
} // namespace xplore
