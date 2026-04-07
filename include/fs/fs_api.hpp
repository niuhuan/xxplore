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

struct FileStatInfo {
    bool     exists      = false;
    bool     isDirectory = false;
    uint64_t size        = 0;
};

struct ImageInfo {
    int width  = 0;
    int height = 0;
};

inline bool isVirtualRoot(const std::string& path) {
    return path.empty() || path == "/";
}

bool pathAllowsSelection(const std::string& path);
std::vector<FileEntry> getRootEntries();
std::vector<FileEntry> listDir(const std::string& path);
std::string parentPath(const std::string& path);
std::string joinPath(const std::string& dir, const std::string& name);
const char* iconForEntry(const FileEntry& entry);
bool statPath(const std::string& path, FileStatInfo& out);
bool isImagePath(const std::string& path);
bool isInstallPackagePath(const std::string& path);
std::string formatSize(uint64_t bytes);
bool probeImageInfo(const std::string& path, ImageInfo& out, std::string& errOut);

bool pathExists(const std::string& path);
bool isDirectoryPath(const std::string& path);
bool isValidEnglishFileName(const std::string& name);

bool createDirectory(const std::string& path, std::string& errOut);
bool removeAll(const std::string& path, std::string& errOut);
bool renamePath(const std::string& from, const std::string& to, std::string& errOut);

/// Callback: current file being processed. Return true to continue, false to interrupt.
using ProgressCb = std::function<bool(const std::string& currentFile)>;

/// Overwrite copy: delete dst then copy src tree.
bool copyEntryOverwrite(const std::string& src, const std::string& dst,
                        std::string& errOut, const ProgressCb& cb);

/// Merge copy: file→delete dst then copy; folder→recurse into existing folder.
bool copyEntryMerge(const std::string& src, const std::string& dst,
                    std::string& errOut, const ProgressCb& cb);

/// Overwrite move: delete dst then rename (or copy+remove).
bool moveEntryOverwrite(const std::string& src, const std::string& dst,
                        std::string& errOut, const ProgressCb& cb);

/// Merge move: file→delete dst then move; folder→recurse merge then remove empty src dir.
bool moveEntryMerge(const std::string& src, const std::string& dst,
                    std::string& errOut, const ProgressCb& cb);

/// Simple copy (no conflict): dst must not exist.
bool copyEntrySimple(const std::string& src, const std::string& dst,
                     std::string& errOut, const ProgressCb& cb);

/// Simple move (no conflict): dst must not exist.
bool moveEntrySimple(const std::string& src, const std::string& dst,
                     std::string& errOut, const ProgressCb& cb);

} // namespace fs
} // namespace xplore
