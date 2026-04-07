#include "fs/fs_api.hpp"
#include <cctype>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>

namespace xplore {
namespace fs {

namespace stdfs = std::filesystem;

static bool removeExistingEntry(const std::string& path, std::string& errOut);

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
    if (stdfs::is_directory(src)) {
        stdfs::create_directories(dst, ec);
        if (ec) {
            errOut = ec.message();
            return false;
        }
        for (const auto& entry : stdfs::directory_iterator(src, ec)) {
            if (ec)
                break;
            stdfs::path target = dst / entry.path().filename();
            if (!copyRecursive(entry.path(), target, overwrite, errOut))
                return false;
        }
        if (ec) {
            errOut = ec.message();
            return false;
        }
    } else {
        // Overwrite = delete destination first; avoid copy_file(overwrite_existing) on Horizon.
        if (overwrite) {
            std::error_code ecEx;
            if (stdfs::exists(dst, ecEx)) {
                if (!removeExistingEntry(dst.string(), errOut))
                    return false;
            }
        }
        stdfs::copy_file(src, dst, stdfs::copy_options::none, ec);
        if (ec) {
            errOut = ec.message();
            return false;
        }
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
    // copy + remove fallback (Horizon often rejects rename across buffers; remove() may fail on files)
    if (!copyRecursive(a, b, true, errOut))
        return false;
    std::error_code ecType;
    const bool      srcIsDir = stdfs::is_directory(a, ecType);
    (void)ecType;
    std::error_code ecRm;
    if (srcIsDir)
        stdfs::remove_all(a, ecRm);
    else
        stdfs::remove(a, ecRm);
    if (ecRm) {
        if (!srcIsDir && ::unlink(src.c_str()) == 0)
            return true;
        errOut = ecRm.message();
        return false;
    }
    return true;
}

} // namespace fs
} // namespace xplore
