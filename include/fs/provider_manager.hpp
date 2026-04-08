#pragma once
#include "fs/file_provider.hpp"
#include "fs/fs_api.hpp"
#include <memory>
#include <string>
#include <vector>

namespace xplore {
namespace fs {

/// Manages registered FileProviders and routes path-based operations.
///
/// Path convention:
///   - "sdmc:/some/path"          → LocalFileProvider
///   - "webdav-{id}:/some/path"   → WebDavProvider with that id
///   - "smb-{id}:/some/path"      → SmbProvider with that id
///   - "/"                        → virtual root (lists all providers + special entries)
///
/// The prefix (everything before the first ":/") identifies the provider.
class ProviderManager {
public:
    ProviderManager();

    /// Register a provider. Ownership is shared.
    void registerProvider(std::shared_ptr<FileProvider> provider);

    /// Remove provider by id.
    void removeProvider(const std::string& providerId);

    /// Find a provider by its id. Returns nullptr if not found.
    FileProvider* findProvider(const std::string& providerId);

    /// Resolve a full path to (provider, relativePath). Returns nullptr if virtual root or unknown.
    FileProvider* resolveProvider(const std::string& fullPath, std::string& outRelativePath);

    /// Build root entries list: all provider display prefixes.
    /// Does NOT include special entries like "Add network address" — caller adds those.
    std::vector<FileEntry> getRootEntries() const;

    /// Whether a path allows file operations (selection, copy, etc.)
    bool pathAllowsSelection(const std::string& fullPath) const;

    /// Whether a path is the virtual root.
    static bool isVirtualRoot(const std::string& path) {
        return path.empty() || path == "/";
    }

    // --- Delegated operations (route to correct provider) ---

    std::vector<FileEntry> listDir(const std::string& fullPath, std::string& errOut);
    bool statPath(const std::string& fullPath, FileStatInfo& out, std::string& errOut);
    bool createDirectory(const std::string& fullPath, std::string& errOut);
    bool removeAll(const std::string& fullPath, std::string& errOut);
    bool renamePath(const std::string& from, const std::string& to, std::string& errOut);
    bool readFile(const std::string& fullPath, uint64_t offset, size_t size,
                  void* outBuffer, std::string& errOut);
    bool writeFile(const std::string& fullPath, const void* data, size_t size,
                   std::string& errOut);
    bool pathExists(const std::string& fullPath);
    bool isDirectoryPath(const std::string& fullPath);

    /// Copy entry, possibly cross-provider. Uses streaming for cross-provider copies.
    bool copyEntry(const std::string& src, const std::string& dst,
                   std::string& errOut, const ProviderProgressCb& cb = nullptr);
    bool moveEntry(const std::string& src, const std::string& dst,
                   std::string& errOut, const ProviderProgressCb& cb = nullptr);

    /// Copy with overwrite/merge strategies (cross-provider aware).
    bool copyEntryOverwrite(const std::string& src, const std::string& dst,
                            std::string& errOut, const ProviderProgressCb& cb = nullptr);
    bool copyEntryMerge(const std::string& src, const std::string& dst,
                        std::string& errOut, const ProviderProgressCb& cb = nullptr);
    bool moveEntryOverwrite(const std::string& src, const std::string& dst,
                            std::string& errOut, const ProviderProgressCb& cb = nullptr);
    bool moveEntryMerge(const std::string& src, const std::string& dst,
                        std::string& errOut, const ProviderProgressCb& cb = nullptr);
    bool copyEntrySimple(const std::string& src, const std::string& dst,
                         std::string& errOut, const ProviderProgressCb& cb = nullptr);
    bool moveEntrySimple(const std::string& src, const std::string& dst,
                         std::string& errOut, const ProviderProgressCb& cb = nullptr);

    /// Check if src and dst are on the same provider.
    bool isSameProvider(const std::string& pathA, const std::string& pathB) const;

    /// Get provider prefix from a full path (e.g. "sdmc:" from "sdmc:/foo").
    static std::string extractPrefix(const std::string& fullPath);

    /// Whether the path points to a network provider (not local).
    bool isNetworkPath(const std::string& fullPath) const;

private:
    std::vector<std::shared_ptr<FileProvider>> providers_;

    /// Cross-provider recursive copy (streaming).
    bool crossProviderCopy(FileProvider* srcProv, const std::string& srcRel,
                           FileProvider* dstProv, const std::string& dstRel,
                           std::string& errOut, const ProviderProgressCb& cb);
};

} // namespace fs
} // namespace xplore
