#include "fs/fs_api.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace xplore {
namespace fs {

bool pathAllowsSelection(const std::string& path) {
    if (isVirtualRoot(path))
        return false;
    // Extend this list when WebDAV/SMB/etc. mounts are implemented.
    static const char* kWritableMountPrefixes[] = {
        "sdmc:/",
        // "webdav:/",
        // "smb:/",
    };
    for (const char* pre : kWritableMountPrefixes) {
        size_t n = strlen(pre);
        if (path.size() >= n && path.compare(0, n, pre) == 0)
            return true;
    }
    return false;
}

std::vector<FileEntry> getRootEntries() {
    return {{"sdmc:", true, 0}};
}

std::vector<FileEntry> listDir(const std::string& path) {
    std::vector<FileEntry> dirs;
    std::vector<FileEntry> files;

    DIR* dp = opendir(path.c_str());
    if (!dp) {
        printf("listDir failed: %s\n", path.c_str());
        return {};
    }

    struct dirent* ep;
    while ((ep = readdir(dp)) != nullptr) {
        if (strcmp(ep->d_name, ".") == 0 || strcmp(ep->d_name, "..") == 0)
            continue;

        FileEntry entry;
        entry.name = ep->d_name;

        std::string fullPath = joinPath(path, entry.name);
        struct stat st;
        if (stat(fullPath.c_str(), &st) == 0) {
            entry.isDirectory = S_ISDIR(st.st_mode);
            entry.size = static_cast<uint64_t>(st.st_size);
        } else {
            entry.isDirectory = (ep->d_type == DT_DIR);
            entry.size = 0;
        }

        if (entry.isDirectory)
            dirs.push_back(std::move(entry));
        else
            files.push_back(std::move(entry));
    }
    closedir(dp);

    auto cmpNoCase = [](const FileEntry& a, const FileEntry& b) {
        const auto& sa = a.name;
        const auto& sb = b.name;
        size_t len = std::min(sa.size(), sb.size());
        for (size_t i = 0; i < len; i++) {
            int ca = std::tolower(static_cast<unsigned char>(sa[i]));
            int cb = std::tolower(static_cast<unsigned char>(sb[i]));
            if (ca != cb) return ca < cb;
        }
        return sa.size() < sb.size();
    };
    std::sort(dirs.begin(), dirs.end(), cmpNoCase);
    std::sort(files.begin(), files.end(), cmpNoCase);

    dirs.insert(dirs.end(),
                std::make_move_iterator(files.begin()),
                std::make_move_iterator(files.end()));
    return dirs;
}

std::string parentPath(const std::string& path) {
    // "sdmc:/" → virtual root
    if (path == "sdmc:/" || path == "sdmc:")
        return "/";

    // Strip trailing slash for the search
    std::string p = path;
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();

    auto pos = p.rfind('/');
    if (pos == std::string::npos)
        return "/";

    // Keep the slash after "sdmc:" → "sdmc:/"
    std::string parent = p.substr(0, pos);
    if (parent == "sdmc:")
        return "sdmc:/";
    if (parent.empty())
        return "/";
    return parent;
}

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty() || dir.back() == '/')
        return dir + name;
    return dir + "/" + name;
}

static std::string toLowerStr(const std::string& s) {
    std::string r = s;
    for (auto& c : r)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

const char* iconForEntry(const FileEntry& entry) {
    if (entry.isDirectory)
        return "folder";

    auto dot = entry.name.rfind('.');
    if (dot == std::string::npos)
        return "file";

    std::string ext = toLowerStr(entry.name.substr(dot));

    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" ||
        ext == ".bmp" || ext == ".webp" || ext == ".svg" || ext == ".ico")
        return "image";
    if (ext == ".mp4" || ext == ".mkv" || ext == ".avi" || ext == ".mov" ||
        ext == ".wmv" || ext == ".flv" || ext == ".webm")
        return "video";
    if (ext == ".mp3" || ext == ".flac" || ext == ".wav" || ext == ".ogg" ||
        ext == ".aac" || ext == ".m4a" || ext == ".wma")
        return "audio";
    if (ext == ".zip" || ext == ".rar" || ext == ".7z" || ext == ".tar" ||
        ext == ".gz" || ext == ".bz2" || ext == ".xz" || ext == ".nsp" ||
        ext == ".xci" || ext == ".nsz" || ext == ".xcz")
        return "archive";
    if (ext == ".txt" || ext == ".md" || ext == ".log" || ext == ".ini" ||
        ext == ".cfg" || ext == ".csv" || ext == ".rtf")
        return "text";
    if (ext == ".js" || ext == ".ts" || ext == ".tsx" || ext == ".jsx" ||
        ext == ".py" || ext == ".c" || ext == ".cpp" || ext == ".h" ||
        ext == ".hpp" || ext == ".json" || ext == ".xml" || ext == ".html" ||
        ext == ".css" || ext == ".yaml" || ext == ".yml" || ext == ".sh" ||
        ext == ".bat" || ext == ".rs" || ext == ".go" || ext == ".java")
        return "code";

    return "file";
}

} // namespace fs
} // namespace xplore
