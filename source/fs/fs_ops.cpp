#include "fs/fs_api.hpp"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <functional>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace xplore {
namespace fs {

namespace stdfs = std::filesystem;

static bool removeExistingEntry(const std::string& path, std::string& errOut);

/// Copy a single file using fopen/fread/fwrite (std::filesystem::copy_file is
/// broken on Horizon — reports "Function not implemented").
static bool copyFilePosix(const std::string& src, const std::string& dst, std::string& errOut) {
    FILE* fin = fopen(src.c_str(), "rb");
    if (!fin) {
        errOut = std::string("open src: ") + strerror(errno);
        return false;
    }
    FILE* fout = fopen(dst.c_str(), "wb");
    if (!fout) {
        errOut = std::string("open dst: ") + strerror(errno);
        fclose(fin);
        return false;
    }
    static constexpr size_t kBuf = 128 * 1024;
    char buf[kBuf];
    bool ok = true;
    while (!feof(fin)) {
        size_t n = fread(buf, 1, kBuf, fin);
        if (ferror(fin)) {
            errOut = std::string("read: ") + strerror(errno);
            ok = false;
            break;
        }
        if (n > 0 && fwrite(buf, 1, n, fout) != n) {
            errOut = std::string("write: ") + strerror(errno);
            ok = false;
            break;
        }
    }
    fclose(fin);
    fclose(fout);
    if (!ok)
        ::unlink(dst.c_str());
    return ok;
}

bool pathExists(const std::string& path) {
    std::error_code ec;
    return stdfs::exists(stdfs::path(path), ec);
}

bool isDirectoryPath(const std::string& path) {
    std::error_code ec;
    return stdfs::is_directory(stdfs::path(path), ec);
}

bool isValidEnglishFileName(const std::string& s) {
    if (s.empty() || s.size() > 255)
        return false;
    if (s == "." || s == "..")
        return false;
    if (s.front() == '.' || s.back() == '.')
        return false;
    for (unsigned char ch : s) {
        if (std::isalnum(ch))
            continue;
        if (ch == '.' || ch == '_' || ch == '-')
            continue;
        return false;
    }
    return true;
}

static bool copyRecursive(const stdfs::path& src, const stdfs::path& dst, bool overwrite,
                          std::string& errOut) {
    std::error_code ec;
    if (!stdfs::exists(src, ec)) {
        errOut = "source missing";
        return false;
    }

    struct stat srcStat;
    if (::stat(src.c_str(), &srcStat) != 0) {
        errOut = "stat failed";
        return false;
    }

    if (S_ISDIR(srcStat.st_mode)) {
        stdfs::create_directories(dst, ec);
        if (ec) {
            errOut = ec.message();
            return false;
        }
        // Use POSIX opendir/readdir — std::filesystem::directory_iterator is
        // unreliable on Horizon and may stop after the first entry.
        DIR* dp = ::opendir(src.c_str());
        if (!dp) {
            errOut = "opendir failed";
            return false;
        }
        struct dirent* ep;
        bool ok = true;
        while ((ep = ::readdir(dp)) != nullptr) {
            if (strcmp(ep->d_name, ".") == 0 || strcmp(ep->d_name, "..") == 0)
                continue;
            stdfs::path childSrc = src / ep->d_name;
            stdfs::path childDst = dst / ep->d_name;
            if (!copyRecursive(childSrc, childDst, overwrite, errOut)) {
                ok = false;
                break;
            }
        }
        ::closedir(dp);
        if (!ok)
            return false;
    } else {
        if (overwrite) {
            std::error_code ecEx;
            if (stdfs::exists(dst, ecEx)) {
                if (!removeExistingEntry(dst.string(), errOut))
                    return false;
            }
        }
        if (!copyFilePosix(src.string(), dst.string(), errOut))
            return false;
    }
    return true;
}

bool createDirectory(const std::string& path, std::string& errOut) {
    std::error_code ec;
    stdfs::create_directories(stdfs::path(path), ec);
    if (ec) {
        errOut = ec.message();
        return false;
    }
    return true;
}

/// Remove a file or directory tree; uses unlink when std::filesystem::remove fails (Horizon quirk).
static bool removeExistingEntry(const std::string& path, std::string& errOut) {
    std::error_code ec;
    stdfs::path     p(path);
    if (!stdfs::exists(p, ec)) {
        if (ec) {
            errOut = ec.message();
            return false;
        }
        return true;
    }
    std::error_code ecType;
    const bool      isDir = stdfs::is_directory(p, ecType);
    (void)ecType;
    if (isDir)
        stdfs::remove_all(p, ec);
    else {
        stdfs::remove(p, ec);
        if (ec && ::unlink(path.c_str()) == 0)
            ec.clear();
    }
    if (ec) {
        errOut = ec.message();
        return false;
    }
    return true;
}

bool removeAll(const std::string& path, std::string& errOut) {
    std::error_code ec;
    stdfs::path p(path);
    if (!stdfs::exists(p, ec)) {
        if (ec) {
            errOut = ec.message();
            return false;
        }
        return true;
    }
    bool isDir = stdfs::is_directory(p, ec);
    if (ec) {
        errOut = ec.message();
        return false;
    }
    if (isDir) {
        (void)stdfs::remove_all(p, ec);
    } else {
        stdfs::remove(p, ec);
        if (ec) {
            if (::unlink(path.c_str()) == 0)
                return true;
            errOut = ec.message();
            return false;
        }
    }
    if (ec) {
        errOut = ec.message();
        return false;
    }
    return true;
}

bool renamePath(const std::string& from, const std::string& to, std::string& errOut) {
    std::error_code ec;
    stdfs::rename(stdfs::path(from), stdfs::path(to), ec);
    if (ec) {
        errOut = ec.message();
        return false;
    }
    return true;
}

bool copyEntry(const std::string& src, const std::string& dst, bool overwrite,
               std::string& errOut) {
    stdfs::path a(src), b(dst);
    std::error_code ec;
    if (stdfs::exists(b, ec)) {
        if (!overwrite) {
            errOut = "exists";
            return false;
        }
        if (!removeExistingEntry(dst, errOut))
            return false;
    }
    return copyRecursive(a, b, overwrite, errOut);
}

bool moveEntry(const std::string& src, const std::string& dst, bool overwrite,
               std::string& errOut) {
    stdfs::path a(src), b(dst);
    std::error_code ec;
    if (stdfs::exists(b, ec)) {
        if (!overwrite) {
            errOut = "exists";
            return false;
        }
        if (!removeExistingEntry(dst, errOut))
            return false;
    }
    stdfs::rename(a, b, ec);
    if (!ec)
        return true;
    if (!copyRecursive(a, b, true, errOut))
        return false;
    return removeAll(src, errOut);
}

// ---------- Callback-based recursive copy ----------

static bool copyRecursiveCb(const stdfs::path& src, const stdfs::path& dst, bool overwrite,
                            std::string& errOut, const ProgressCallback& onProgress) {
    std::error_code ec;
    if (!stdfs::exists(src, ec)) {
        errOut = "source missing";
        return false;
    }

    struct stat srcStat;
    if (::stat(src.c_str(), &srcStat) != 0) {
        errOut = "stat failed";
        return false;
    }

    if (S_ISDIR(srcStat.st_mode)) {
        if (onProgress) onProgress(dst.string());
        stdfs::create_directories(dst, ec);
        if (ec) {
            errOut = ec.message();
            return false;
        }
        DIR* dp = ::opendir(src.c_str());
        if (!dp) {
            errOut = "opendir failed";
            return false;
        }
        struct dirent* ep;
        bool ok = true;
        while ((ep = ::readdir(dp)) != nullptr) {
            if (strcmp(ep->d_name, ".") == 0 || strcmp(ep->d_name, "..") == 0)
                continue;
            stdfs::path childSrc = src / ep->d_name;
            stdfs::path childDst = dst / ep->d_name;
            if (!copyRecursiveCb(childSrc, childDst, overwrite, errOut, onProgress)) {
                ok = false;
                break;
            }
        }
        ::closedir(dp);
        if (!ok) return false;
    } else {
        if (onProgress) onProgress(dst.string());
        if (overwrite) {
            std::error_code ecEx;
            if (stdfs::exists(dst, ecEx)) {
                if (!removeExistingEntry(dst.string(), errOut))
                    return false;
            }
        }
        if (!copyFilePosix(src.string(), dst.string(), errOut))
            return false;
    }
    return true;
}

bool copyEntryWithProgress(const std::string& src, const std::string& dst, bool overwrite,
                           std::string& errOut, const ProgressCallback& onProgress) {
    stdfs::path a(src), b(dst);
    std::error_code ec;
    if (stdfs::exists(b, ec)) {
        if (!overwrite) {
            errOut = "exists";
            return false;
        }
        if (!removeExistingEntry(dst, errOut))
            return false;
    }
    return copyRecursiveCb(a, b, overwrite, errOut, onProgress);
}

bool moveEntryWithProgress(const std::string& src, const std::string& dst, bool overwrite,
                           std::string& errOut, const ProgressCallback& onProgress) {
    stdfs::path a(src), b(dst);
    std::error_code ec;
    if (stdfs::exists(b, ec)) {
        if (!overwrite) {
            errOut = "exists";
            return false;
        }
        if (!removeExistingEntry(dst, errOut))
            return false;
    }
    stdfs::rename(a, b, ec);
    if (!ec)
        return true;
    if (!copyRecursiveCb(a, b, true, errOut, onProgress))
        return false;
    return removeAll(src, errOut);
}

} // namespace fs
} // namespace xplore
