#include "fs/fs_api.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>

namespace xxplore {
namespace fs {

namespace {

std::string toLowerStr(const std::string& s) {
    std::string r = s;
    for (auto& c : r)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

std::string pathExtensionLower(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    size_t dot   = path.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return {};
    return toLowerStr(path.substr(dot));
}

uint16_t readBe16(const unsigned char* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t readBe32(const unsigned char* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

uint16_t readLe16(const unsigned char* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t readLe32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

int32_t readLe32Signed(const unsigned char* p) {
    return static_cast<int32_t>(readLe32(p));
}

bool probePngInfo(FILE* f, ImageInfo& out, std::string& errOut) {
    unsigned char buf[24];
    if (fread(buf, 1, sizeof(buf), f) != sizeof(buf)) {
        errOut = "read png header failed";
        return false;
    }
    static const unsigned char kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (memcmp(buf, kSig, sizeof(kSig)) != 0) {
        errOut = "invalid png signature";
        return false;
    }
    out.width  = static_cast<int>(readBe32(buf + 16));
    out.height = static_cast<int>(readBe32(buf + 20));
    return out.width > 0 && out.height > 0;
}

bool probeGifInfo(FILE* f, ImageInfo& out, std::string& errOut) {
    unsigned char buf[10];
    if (fread(buf, 1, sizeof(buf), f) != sizeof(buf)) {
        errOut = "read gif header failed";
        return false;
    }
    if (memcmp(buf, "GIF87a", 6) != 0 && memcmp(buf, "GIF89a", 6) != 0) {
        errOut = "invalid gif signature";
        return false;
    }
    out.width  = static_cast<int>(readLe16(buf + 6));
    out.height = static_cast<int>(readLe16(buf + 8));
    return out.width > 0 && out.height > 0;
}

bool probeBmpInfo(FILE* f, ImageInfo& out, std::string& errOut) {
    unsigned char buf[32];
    if (fread(buf, 1, sizeof(buf), f) < 26) {
        errOut = "read bmp header failed";
        return false;
    }
    if (buf[0] != 'B' || buf[1] != 'M') {
        errOut = "invalid bmp signature";
        return false;
    }
    uint32_t dibSize = readLe32(buf + 14);
    if (dibSize == 12) {
        out.width  = static_cast<int>(readLe16(buf + 18));
        out.height = static_cast<int>(readLe16(buf + 20));
    } else {
        out.width  = readLe32Signed(buf + 18);
        int32_t h  = readLe32Signed(buf + 22);
        out.height = h < 0 ? -h : h;
    }
    return out.width > 0 && out.height > 0;
}

bool probeJpegInfo(FILE* f, ImageInfo& out, std::string& errOut) {
    unsigned char marker[2];
    if (fread(marker, 1, 2, f) != 2 || marker[0] != 0xff || marker[1] != 0xd8) {
        errOut = "invalid jpeg signature";
        return false;
    }

    for (;;) {
        int c = fgetc(f);
        while (c == 0xff)
            c = fgetc(f);
        if (c == EOF) {
            errOut = "unexpected jpeg eof";
            return false;
        }

        if (c == 0xd9 || c == 0xda) {
            errOut = "jpeg size marker not found";
            return false;
        }

        unsigned char lenBuf[2];
        if (fread(lenBuf, 1, 2, f) != 2) {
            errOut = "read jpeg segment failed";
            return false;
        }
        uint16_t segLen = readBe16(lenBuf);
        if (segLen < 2) {
            errOut = "invalid jpeg segment";
            return false;
        }

        if ((c >= 0xc0 && c <= 0xc3) || (c >= 0xc5 && c <= 0xc7) ||
            (c >= 0xc9 && c <= 0xcb) || (c >= 0xcd && c <= 0xcf)) {
            unsigned char sof[5];
            if (fread(sof, 1, sizeof(sof), f) != sizeof(sof)) {
                errOut = "read jpeg sof failed";
                return false;
            }
            out.height = static_cast<int>(readBe16(sof + 1));
            out.width  = static_cast<int>(readBe16(sof + 3));
            return out.width > 0 && out.height > 0;
        }

        if (fseek(f, static_cast<long>(segLen) - 2L, SEEK_CUR) != 0) {
            errOut = "seek jpeg segment failed";
            return false;
        }
    }
}

} // namespace

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
    return {{"sdmc:", true, 0, false}};
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
            entry.hasSize = !entry.isDirectory;
        } else {
            entry.isDirectory = (ep->d_type == DT_DIR);
            entry.size = 0;
            entry.hasSize = false;
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
    // Strip trailing slash for the search
    std::string p = path;
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();

    // Find the "prefix:" part (e.g. "sdmc:", "webdav-abc123:")
    auto colonPos = p.find(':');

    // "sdmc:/" or "webdav-abc123:/" → virtual root
    if (colonPos != std::string::npos && colonPos + 1 >= p.size())
        return "/";
    // "prefix:" with no path → virtual root
    if (colonPos != std::string::npos && p.size() == colonPos + 1)
        return "/";

    auto pos = p.rfind('/');
    if (pos == std::string::npos)
        return "/";

    // Keep the slash after "prefix:" → "prefix:/"
    std::string parent = p.substr(0, pos);
    if (colonPos != std::string::npos && parent.size() == colonPos + 1) {
        // parent is "prefix:" → return "prefix:/"
        return parent + "/";
    }
    if (parent.empty())
        return "/";
    return parent;
}

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty() || dir.back() == '/')
        return dir + name;
    return dir + "/" + name;
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

bool statPath(const std::string& path, FileStatInfo& out) {
    struct stat st;
    memset(&st, 0, sizeof(st));
    if (stat(path.c_str(), &st) != 0) {
        out = {};
        return false;
    }
    out.exists      = true;
    out.isDirectory = S_ISDIR(st.st_mode);
    out.size        = static_cast<uint64_t>(st.st_size);
    return true;
}

bool isImagePath(const std::string& path) {
    const std::string ext = pathExtensionLower(path);
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
           ext == ".gif" || ext == ".bmp";
}

bool isInstallPackagePath(const std::string& path) {
    const std::string ext = pathExtensionLower(path);
    return ext == ".nsp" || ext == ".nsz" || ext == ".xci" || ext == ".xcz";
}

bool isTextEditorPath(const std::string& path) {
    const std::string ext = pathExtensionLower(path);
    return ext == ".txt" || ext == ".md" || ext == ".log" || ext == ".ini" ||
           ext == ".cfg" || ext == ".conf" || ext == ".yaml" || ext == ".yml" ||
           ext == ".json" || ext == ".xml" || ext == ".toml" || ext == ".csv" ||
           ext == ".tsv" || ext == ".rtf" || ext == ".html" || ext == ".htm" ||
           ext == ".css" || ext == ".js" || ext == ".ts" || ext == ".jsx" ||
           ext == ".tsx" || ext == ".py" || ext == ".sh" || ext == ".bat" ||
           ext == ".ps1" || ext == ".c" || ext == ".cc" || ext == ".cpp" ||
           ext == ".h" || ext == ".hh" || ext == ".hpp" || ext == ".java" ||
           ext == ".kt" || ext == ".rs" || ext == ".go" || ext == ".sql";
}

bool isZipFilePath(const std::string& path) {
    if (isZipBrowsePath(path))
        return false;
    return pathExtensionLower(path) == ".zip";
}

bool splitZipBrowsePath(const std::string& path, std::string& outerPath, std::string& innerPath) {
    outerPath.clear();
    innerPath.clear();

    const std::size_t marker = path.rfind(".zip:/");
    if (marker == std::string::npos)
        return false;

    outerPath = path.substr(0, marker + 4);
    innerPath = path.substr(marker + 4);
    if (outerPath.empty() || innerPath.empty() || innerPath[0] != ':')
        return false;
    innerPath.erase(0, 1);
    if (innerPath.empty())
        innerPath = "/";
    return true;
}

bool isZipBrowsePath(const std::string& path) {
    std::string outerPath;
    std::string innerPath;
    return splitZipBrowsePath(path, outerPath, innerPath);
}

std::string formatSize(uint64_t bytes) {
    static const char* kUnits[] = {"B", "K", "M", "G", "T"};

    auto addThousands = [](const std::string& digits) {
        if (digits.empty())
            return digits;
        std::string out;
        out.reserve(digits.size() + digits.size() / 3);
        int count = 0;
        for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
            if (count > 0 && count % 3 == 0)
                out.push_back(',');
            out.push_back(*it);
            ++count;
        }
        std::reverse(out.begin(), out.end());
        return out;
    };

    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }

    if (unit == 0)
        return addThousands(std::to_string(static_cast<unsigned long long>(bytes))) + ".0 B";

    char raw[64];
    std::snprintf(raw, sizeof(raw), "%.1f", value);
    std::string formatted(raw);
    std::size_t dot = formatted.find('.');
    if (dot == std::string::npos)
        return addThousands(formatted) + " " + kUnits[unit];

    std::string integerPart = formatted.substr(0, dot);
    std::string decimalPart = formatted.substr(dot);
    std::string withComma = addThousands(integerPart) + decimalPart;

    if (withComma == "1024.0" && unit < 4) {
        value /= 1024.0;
        ++unit;
        std::snprintf(raw, sizeof(raw), "%.1f", value);
        formatted = raw;
        dot = formatted.find('.');
        integerPart = formatted.substr(0, dot);
        decimalPart = formatted.substr(dot);
        withComma = addThousands(integerPart) + decimalPart;
    }

    return withComma + " " + kUnits[unit];
}

bool probeImageInfo(const std::string& path, ImageInfo& out, std::string& errOut) {
    out = {};

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        errOut = "open image failed";
        return false;
    }

    std::string ext = pathExtensionLower(path);
    bool ok         = false;
    if (ext == ".png")
        ok = probePngInfo(f, out, errOut);
    else if (ext == ".gif")
        ok = probeGifInfo(f, out, errOut);
    else if (ext == ".bmp")
        ok = probeBmpInfo(f, out, errOut);
    else if (ext == ".jpg" || ext == ".jpeg")
        ok = probeJpegInfo(f, out, errOut);
    else
        errOut = "unsupported image format";

    fclose(f);
    if (!ok && errOut.empty())
        errOut = "probe image failed";
    return ok;
}

} // namespace fs
} // namespace xxplore
