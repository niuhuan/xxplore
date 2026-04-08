#pragma once
#include "fs/fs_api.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace xplore {
namespace fs {

/// Callback for long operations. Return true to continue, false to interrupt.
using ProviderProgressCb = std::function<bool(const std::string& currentFile)>;

/// Abstract file system provider. Implementations wrap local FS, WebDAV, SMB2, etc.
class FileProvider {
public:
    virtual ~FileProvider() = default;

    /// Unique identifier for this provider instance (e.g. "local", "webdav-abc123").
    virtual std::string providerId() const = 0;

    /// Display prefix shown in root/breadcrumb (e.g. "sdmc:", "MyNAS(WebDAV):").
    virtual std::string displayPrefix() const = 0;

    /// Whether the provider is read-only.
    virtual bool isReadOnly() const = 0;

    /// Whether paths under this provider support multi-selection for file ops.
    virtual bool allowsSelection() const = 0;

    /// List directory contents. @p path is relative to provider root (e.g. "/" means provider root).
    /// Returns empty on error (check errOut).
    virtual std::vector<FileEntry> listDir(const std::string& path,
                                           std::string& errOut) = 0;

    /// Stat a single path. Returns false on error.
    virtual bool statPath(const std::string& path, FileStatInfo& out,
                          std::string& errOut) = 0;

    /// Create a directory.
    virtual bool createDirectory(const std::string& path, std::string& errOut) = 0;

    /// Recursively delete a file or directory.
    virtual bool removeAll(const std::string& path, std::string& errOut) = 0;

    /// Rename / move within the same provider.
    virtual bool renamePath(const std::string& from, const std::string& to,
                            std::string& errOut) = 0;

    /// Read a range of bytes from a file. Used by installer readRange and cross-provider copy.
    virtual bool readFile(const std::string& path, uint64_t offset, size_t size,
                          void* outBuffer, std::string& errOut) = 0;

    /// Write full file from buffer. Creates or overwrites.
    virtual bool writeFile(const std::string& path, const void* data, size_t size,
                           std::string& errOut) = 0;

    /// Copy a single file within this provider. Default implementation uses readFile+writeFile.
    virtual bool copyFile(const std::string& src, const std::string& dst,
                          std::string& errOut, const ProviderProgressCb& cb = nullptr);

    /// Move a single file within this provider. Default implementation: copy + remove.
    virtual bool moveFile(const std::string& src, const std::string& dst,
                          std::string& errOut, const ProviderProgressCb& cb = nullptr);

    /// Copy an entire entry (file or dir tree) within this provider.
    /// Default: recursive readFile/writeFile. Overrides can use native commands.
    virtual bool copyEntry(const std::string& src, const std::string& dst,
                           std::string& errOut, const ProviderProgressCb& cb = nullptr);

    /// Move an entire entry (file or dir tree) within this provider.
    virtual bool moveEntry(const std::string& src, const std::string& dst,
                           std::string& errOut, const ProviderProgressCb& cb = nullptr);

    /// Check if a path exists.
    virtual bool pathExists(const std::string& path);

    /// Check if a path is a directory.
    virtual bool isDirectory(const std::string& path);

    /// Get file size. Returns 0 on error.
    virtual uint64_t fileSize(const std::string& path);
};

} // namespace fs
} // namespace xplore
