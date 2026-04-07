#pragma once
#include <cstdint>
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

} // namespace fs
} // namespace xplore
