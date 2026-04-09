#pragma once
#include "fs/file_provider.hpp"

namespace xxplore::fs {

/// FileProvider backed by a libusbhsfs-mounted devoptab prefix (e.g. "ums0:").
class UsbMountProvider : public FileProvider {
public:
    UsbMountProvider(std::string providerId, std::string mountPrefix, bool readOnly)
        : providerId_(std::move(providerId)), mountPrefix_(std::move(mountPrefix)),
          readOnly_(readOnly) {}

    ProviderKind kind() const override { return ProviderKind::Usb; }
    std::string providerId() const override { return providerId_; }
    std::string displayPrefix() const override { return mountPrefix_; }
    bool isReadOnly() const override { return readOnly_; }
    bool allowsSelection() const override { return true; }

    std::vector<FileEntry> listDir(const std::string& path, std::string& errOut) override;
    bool statPath(const std::string& path, FileStatInfo& out, std::string& errOut) override;
    bool createDirectory(const std::string& path, std::string& errOut) override;
    bool removeAll(const std::string& path, std::string& errOut) override;
    bool renamePath(const std::string& from, const std::string& to, std::string& errOut) override;
    bool readFile(const std::string& path, uint64_t offset, size_t size,
                  void* outBuffer, std::string& errOut) override;
    bool writeFile(const std::string& path, const void* data, size_t size,
                   std::string& errOut) override;
    bool supportsPartialWrite() const override { return true; }
    bool writeFileChunk(const std::string& path, uint64_t offset,
                        const void* data, size_t size, bool truncate,
                        std::string& errOut) override;

    bool copyFile(const std::string& src, const std::string& dst,
                  std::string& errOut, const ProviderProgressCb& cb) override;
    bool moveFile(const std::string& src, const std::string& dst,
                  std::string& errOut, const ProviderProgressCb& cb) override;
    bool copyEntry(const std::string& src, const std::string& dst,
                   std::string& errOut, const ProviderProgressCb& cb) override;
    bool moveEntry(const std::string& src, const std::string& dst,
                   std::string& errOut, const ProviderProgressCb& cb) override;

private:
    std::string toFull(const std::string& relPath) const;

    std::string providerId_;
    std::string mountPrefix_;
    bool        readOnly_ = false;
};

} // namespace xxplore::fs
