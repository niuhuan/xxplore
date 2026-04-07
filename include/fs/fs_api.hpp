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
