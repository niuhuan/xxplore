#pragma once
#include "fs/file_provider.hpp"
#include <string>

namespace xplore {
namespace fs {

/// WebDAV network file system provider using libcurl.
class WebDavProvider : public FileProvider {
public:
    /// @param id     Unique provider id (e.g. "webdav-abc123")
    /// @param name   Display name (e.g. "MyNAS")
    /// @param url    Base URL (e.g. "http://192.168.1.1/dav")
    /// @param user   Username (may be empty)
    /// @param pass   Password (may be empty)
    WebDavProvider(std::string id, std::string name, std::string url,
                   std::string user, std::string pass);
    ~WebDavProvider() override;

    std::string providerId() const override { return id_; }
    std::string displayPrefix() const override;
    bool isReadOnly() const override { return false; }
    bool allowsSelection() const override { return true; }

    std::vector<FileEntry> listDir(const std::string& path, std::string& errOut) override;
    bool statPath(const std::string& path, FileStatInfo& out, std::string& errOut) override;
    bool createDirectory(const std::string& path, std::string& errOut) override;
    bool removeAll(const std::string& path, std::string& errOut) override;
    bool renamePath(const std::string& from, const std::string& to,
                    std::string& errOut) override;
    bool readFile(const std::string& path, uint64_t offset, size_t size,
                  void* outBuffer, std::string& errOut) override;
    bool writeFile(const std::string& path, const void* data, size_t size,
                   std::string& errOut) override;

    /// Test connectivity (PROPFIND on root). Returns true if successful.
    bool testConnection(std::string& errOut);

private:
    std::string id_;
    std::string name_;
    std::string baseUrl_;   // Normalized: no trailing slash
    std::string user_;
    std::string pass_;

    /// Build full URL for a relative path.
    std::string makeUrl(const std::string& relPath) const;

    /// Common curl setup: auth, timeout, etc.
    struct CurlResult {
        long httpCode = 0;
        std::string body;
        std::string error;
    };
    CurlResult performPropfind(const std::string& url, int depth);
    CurlResult performRequest(const std::string& method, const std::string& url,
                              const void* sendData = nullptr, size_t sendSize = 0);
    CurlResult performGet(const std::string& url, uint64_t offset, size_t size);

    /// Parse PROPFIND XML response into entries.
    std::vector<FileEntry> parsePropfindResponse(const std::string& xml,
                                                 const std::string& basePath);
};

} // namespace fs
} // namespace xplore
