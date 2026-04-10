#pragma once
#include "fs/fs_api.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace xxplore {
namespace fs {

/// Callback for long operations. Return true to continue, false to interrupt.
using ProviderProgressCb = std::function<bool(const std::string& currentFile)>;

enum class ProviderKind {
    Local,
    WebDav,
    Smb,
    Ftp,
    Usb,
};

struct ProviderCapabilities {
    bool canReadRange = false;
    bool canReadSequential = false;
    bool canWrite = false;
    bool canPartialWrite = false;
    bool canCreateDirectory = false;
    bool canDelete = false;
    bool canRename = false;
    bool usesUtf8Paths = true;
    bool canInstallFromSource = false;
};

class SequentialFileReader {
public:
    virtual ~SequentialFileReader() = default;
    virtual bool read(void* outBuffer, size_t size, std::string& errOut) = 0;
};

/// Abstract file system provider. Implementations wrap local FS, WebDAV, SMB2, etc.
class FileProvider {
public:
    virtual ~FileProvider() = default;

    virtual ProviderKind kind() const = 0;

    /// Unique identifier for this provider instance (e.g. "local", "webdav-abc123").
    virtual std::string providerId() const = 0;

    /// Display prefix shown in root/breadcrumb (e.g. "sdmc:", "MyNAS(WebDAV):").
    virtual std::string displayPrefix() const = 0;

    /// Whether the provider is read-only.
    virtual bool isReadOnly() const = 0;

    /// Whether paths under this provider support multi-selection for file ops.
    virtual bool allowsSelection() const = 0;

    /// Provider feature summary used by higher layers to adapt UI and transfer strategy.
    virtual ProviderCapabilities capabilities() const;

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

    /// Open a sequential reader starting at @p offset.
    /// The default implementation wraps readFile() so existing providers remain compatible.
    virtual std::unique_ptr<SequentialFileReader>
    openSequentialRead(const std::string& path, uint64_t offset, std::string& errOut);

    /// Write full file from buffer. Creates or overwrites.
    virtual bool writeFile(const std::string& path, const void* data, size_t size,
                           std::string& errOut) = 0;

    /// Whether this provider supports chunked writes at arbitrary offsets.
    virtual bool supportsPartialWrite() const { return false; }

    /// Write a chunk to a file at a specific offset.
    /// When @p truncate is true, the destination must be recreated/truncated first.
    virtual bool writeFileChunk(const std::string& path, uint64_t offset,
                                const void* data, size_t size, bool truncate,
                                std::string& errOut);

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
} // namespace xxplore
