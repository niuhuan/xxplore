#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace xplore {
namespace fs {

struct FileEntry {
    std::string name;
    bool        isDirectory = false;
    uint64_t    size        = 0;
};

/// Virtual root path that lists mount points (sdmc:).
inline bool isVirtualRoot(const std::string& path) {
    return path.empty() || path == "/";
}

/// True when the panel at @p path may use multi-select and local file ops.
/// Virtual root is never allowed. Currently only SD card paths (`sdmc:/…`)
/// return true; add WebDAV/SMB prefixes here when those mounts are wired up.
bool pathAllowsSelection(const std::string& path);

/// Return the root entries (currently only sdmc:).
std::vector<FileEntry> getRootEntries();

/// List a real directory. Directories come first, then files; both groups
/// are sorted case-insensitively by name.  Returns empty on error.
std::vector<FileEntry> listDir(const std::string& path);

/// Return the parent path. For "sdmc:/" returns "/" (virtual root).
std::string parentPath(const std::string& path);

/// Join a directory path with a child name.
std::string joinPath(const std::string& dir, const std::string& name);

/// Map a filename extension to an icon name (folder, file, image, ...).
/// Returns "folder" for directories, otherwise matches the extension table.
const char* iconForEntry(const FileEntry& entry);

// --- path checks & ASCII filename rules (rename / new folder) ---

bool pathExists(const std::string& path);
bool isDirectoryPath(const std::string& path);

/// Allowed: [A-Za-z0-9._-]+, length <= 255, not "." or "..", no leading/trailing '.'.
bool isValidEnglishFileName(const std::string& name);

// --- mutations (UTF-8 paths; names validated separately where needed) ---

bool createDirectory(const std::string& path, std::string& errOut);
bool removeAll(const std::string& path, std::string& errOut);
bool renamePath(const std::string& from, const std::string& to, std::string& errOut);
/// Copy file or directory tree. If @p dst exists and overwrite=false, fails.
bool copyEntry(const std::string& src, const std::string& dst, bool overwrite,
               std::string& errOut);
/// Rename, or copy+remove if rename fails (e.g. cross-device).
bool moveEntry(const std::string& src, const std::string& dst, bool overwrite,
               std::string& errOut);

/// Callback invoked before each file/dir copy. Argument is the destination path.
using ProgressCallback = std::function<void(const std::string& currentFile)>;

/// Copy file or directory tree with per-file progress callback.
bool copyEntryWithProgress(const std::string& src, const std::string& dst, bool overwrite,
                           std::string& errOut, const ProgressCallback& onProgress);

/// Move entry with per-file progress callback (rename or copy+remove fallback).
bool moveEntryWithProgress(const std::string& src, const std::string& dst, bool overwrite,
                           std::string& errOut, const ProgressCallback& onProgress);

} // namespace fs
} // namespace xplore
