#pragma once

#include "fs/file_provider.hpp"
#include <string>

namespace xxplore {
namespace fs {

class FtpProvider : public FileProvider {
public:
    FtpProvider(std::string id, std::string name, std::string address,
                std::string user, std::string pass);

    ProviderKind kind() const override { return ProviderKind::Ftp; }
    std::string providerId() const override { return id_; }
    std::string displayPrefix() const override;
    bool isReadOnly() const override { return false; }
    bool allowsSelection() const override { return true; }
    ProviderCapabilities capabilities() const override;

    std::vector<FileEntry> listDir(const std::string& path, std::string& errOut) override;
    bool statPath(const std::string& path, FileStatInfo& out, std::string& errOut) override;
    bool createDirectory(const std::string& path, std::string& errOut) override;
    bool removeAll(const std::string& path, std::string& errOut) override;
    bool renamePath(const std::string& from, const std::string& to,
                    std::string& errOut) override;
    bool readFile(const std::string& path, uint64_t offset, size_t size,
                  void* outBuffer, std::string& errOut) override;
    std::unique_ptr<SequentialFileReader>
    openSequentialRead(const std::string& path, uint64_t offset, std::string& errOut) override;
    bool writeFile(const std::string& path, const void* data, size_t size,
                   std::string& errOut) override;
    bool supportsPartialWrite() const override { return true; }
    bool writeFileChunk(const std::string& path, uint64_t offset,
                        const void* data, size_t size, bool truncate,
                        std::string& errOut) override;

private:
    std::string id_;
    std::string name_;
    std::string address_;
    std::string user_;
    std::string pass_;
};

} // namespace fs
} // namespace xxplore
