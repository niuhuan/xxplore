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

// ---- low-level helpers ----

static bool copyFilePosix(const std::string& src, const std::string& dst,
                          std::string& errOut, const ProgressCb& cb = nullptr) {
    FILE* fin = fopen(src.c_str(), "rb");
    if (!fin) { errOut = std::string("open src: ") + strerror(errno); return false; }
    FILE* fout = fopen(dst.c_str(), "wb");
    if (!fout) { errOut = std::string("open dst: ") + strerror(errno); fclose(fin); return false; }
    static constexpr size_t kBuf = 128 * 1024;
    char buf[kBuf];
    bool ok = true;
    while (!feof(fin)) {
        if (cb && !cb(dst)) { errOut = "interrupted"; ok = false; break; }
        size_t n = fread(buf, 1, kBuf, fin);
        if (ferror(fin)) { errOut = std::string("read: ") + strerror(errno); ok = false; break; }
        if (n > 0 && fwrite(buf, 1, n, fout) != n) { errOut = std::string("write: ") + strerror(errno); ok = false; break; }
    }
    fclose(fin);
    fclose(fout);
    if (!ok) ::unlink(dst.c_str());
    return ok;
}

static bool removeEntry(const std::string& path, std::string& errOut) {
    std::error_code ec;
    stdfs::path p(path);
    if (!stdfs::exists(p, ec)) return true;
    std::error_code ecT;
    bool isDir = stdfs::is_directory(p, ecT);
    (void)ecT;
    if (isDir)
        stdfs::remove_all(p, ec);
    else {
        stdfs::remove(p, ec);
        if (ec && ::unlink(path.c_str()) == 0) ec.clear();
    }
    if (ec) { errOut = ec.message(); return false; }
    return true;
}

static bool isDirEntry(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

// ---- public path utilities (unchanged) ----

bool pathExists(const std::string& path) {
    std::error_code ec;
    return stdfs::exists(stdfs::path(path), ec);
}

bool isDirectoryPath(const std::string& path) {
    std::error_code ec;
    return stdfs::is_directory(stdfs::path(path), ec);
}

bool isValidEnglishFileName(const std::string& s) {
    if (s.empty() || s.size() > 255) return false;
    if (s == "." || s == "..") return false;
    if (s.front() == '.' || s.back() == '.') return false;
    for (unsigned char ch : s) {
        if (std::isalnum(ch)) continue;
        if (ch == '.' || ch == '_' || ch == '-') continue;
        return false;
    }
    return true;
}

bool createDirectory(const std::string& path, std::string& errOut) {
    std::error_code ec;
    stdfs::create_directories(stdfs::path(path), ec);
    if (ec) { errOut = ec.message(); return false; }
    return true;
}

bool removeAll(const std::string& path, std::string& errOut) {
    return removeEntry(path, errOut);
}

bool renamePath(const std::string& from, const std::string& to, std::string& errOut) {
    std::error_code ec;
    stdfs::rename(stdfs::path(from), stdfs::path(to), ec);
    if (ec) { errOut = ec.message(); return false; }
    return true;
}

// ---- recursive copy (always overwrites files at leaf) ----

static bool copyTreeCb(const std::string& src, const std::string& dst,
                       std::string& errOut, const ProgressCb& cb) {
    if (cb && !cb(dst)) { errOut = "interrupted"; return false; }
    if (isDirEntry(src)) {
        std::error_code ec;
        stdfs::create_directories(stdfs::path(dst), ec);
        if (ec) { errOut = ec.message(); return false; }
        DIR* dp = ::opendir(src.c_str());
        if (!dp) { errOut = "opendir failed"; return false; }
        struct dirent* ep;
        bool ok = true;
        while ((ep = ::readdir(dp)) != nullptr) {
            if (strcmp(ep->d_name, ".") == 0 || strcmp(ep->d_name, "..") == 0) continue;
            std::string cs = joinPath(src, ep->d_name);
            std::string cd = joinPath(dst, ep->d_name);
            if (!copyTreeCb(cs, cd, errOut, cb)) { ok = false; break; }
        }
        ::closedir(dp);
        return ok;
    }
    // leaf file: remove dst if exists, then copy
    if (pathExists(dst)) {
        if (!removeEntry(dst, errOut)) return false;
    }
    return copyFilePosix(src, dst, errOut, cb);
}

// ---- merge copy ----

static bool mergeCopyCb(const std::string& src, const std::string& dst,
                        std::string& errOut, const ProgressCb& cb) {
    if (cb && !cb(dst)) { errOut = "interrupted"; return false; }
    bool srcIsDir = isDirEntry(src);
    bool dstExists = pathExists(dst);

    if (srcIsDir) {
        if (dstExists && isDirEntry(dst)) {
            // Both dirs: recurse into children
            DIR* dp = ::opendir(src.c_str());
            if (!dp) { errOut = "opendir failed"; return false; }
            struct dirent* ep;
            bool ok = true;
            while ((ep = ::readdir(dp)) != nullptr) {
                if (strcmp(ep->d_name, ".") == 0 || strcmp(ep->d_name, "..") == 0) continue;
                std::string cs = joinPath(src, ep->d_name);
                std::string cd = joinPath(dst, ep->d_name);
                if (!mergeCopyCb(cs, cd, errOut, cb)) { ok = false; break; }
            }
            ::closedir(dp);
            return ok;
        }
        // dst is file or doesn't exist: remove dst, then copy tree
        if (dstExists) {
            if (!removeEntry(dst, errOut)) return false;
        }
        return copyTreeCb(src, dst, errOut, cb);
    }

    // src is file
    if (dstExists) {
        if (!removeEntry(dst, errOut)) return false;
    }
    return copyFilePosix(src, dst, errOut, cb);
}

// ---- merge move ----

static bool mergeMoveFileOrTree(const std::string& src, const std::string& dst,
                                std::string& errOut, const ProgressCb& cb);

static bool mergeMoveCb(const std::string& src, const std::string& dst,
                        std::string& errOut, const ProgressCb& cb) {
    if (cb && !cb(dst)) { errOut = "interrupted"; return false; }
    bool srcIsDir = isDirEntry(src);
    bool dstExists = pathExists(dst);

    if (srcIsDir) {
        if (dstExists && isDirEntry(dst)) {
            // Both dirs: recurse children, then remove empty src dir
            DIR* dp = ::opendir(src.c_str());
            if (!dp) { errOut = "opendir failed"; return false; }
            struct dirent* ep;
            bool ok = true;
            while ((ep = ::readdir(dp)) != nullptr) {
                if (strcmp(ep->d_name, ".") == 0 || strcmp(ep->d_name, "..") == 0) continue;
                std::string cs = joinPath(src, ep->d_name);
                std::string cd = joinPath(dst, ep->d_name);
                if (!mergeMoveCb(cs, cd, errOut, cb)) { ok = false; break; }
            }
            ::closedir(dp);
            if (ok) removeEntry(src, errOut);
            return ok;
        }
        // dst is file or doesn't exist
        if (dstExists) {
            if (!removeEntry(dst, errOut)) return false;
        }
        return mergeMoveFileOrTree(src, dst, errOut, cb);
    }

    // src is file
    if (dstExists) {
        if (!removeEntry(dst, errOut)) return false;
    }
    return mergeMoveFileOrTree(src, dst, errOut, cb);
}

static bool mergeMoveFileOrTree(const std::string& src, const std::string& dst,
                                std::string& errOut, const ProgressCb& cb) {
    // Try rename first
    std::error_code ec;
    stdfs::rename(stdfs::path(src), stdfs::path(dst), ec);
    if (!ec) return true;
    // Fallback: copy + remove
    if (!copyTreeCb(src, dst, errOut, cb)) return false;
    return removeEntry(src, errOut);
}

// ---- public API ----

bool copyEntryOverwrite(const std::string& src, const std::string& dst,
                        std::string& errOut, const ProgressCb& cb) {
    if (pathExists(dst)) {
        if (!removeEntry(dst, errOut)) return false;
    }
    return copyTreeCb(src, dst, errOut, cb);
}

bool copyEntryMerge(const std::string& src, const std::string& dst,
                    std::string& errOut, const ProgressCb& cb) {
    return mergeCopyCb(src, dst, errOut, cb);
}

bool moveEntryOverwrite(const std::string& src, const std::string& dst,
                        std::string& errOut, const ProgressCb& cb) {
    if (pathExists(dst)) {
        if (!removeEntry(dst, errOut)) return false;
    }
    std::error_code ec;
    stdfs::rename(stdfs::path(src), stdfs::path(dst), ec);
    if (!ec) return true;
    if (!copyTreeCb(src, dst, errOut, cb)) return false;
    return removeEntry(src, errOut);
}

bool moveEntryMerge(const std::string& src, const std::string& dst,
                    std::string& errOut, const ProgressCb& cb) {
    return mergeMoveCb(src, dst, errOut, cb);
}

bool copyEntrySimple(const std::string& src, const std::string& dst,
                     std::string& errOut, const ProgressCb& cb) {
    return copyTreeCb(src, dst, errOut, cb);
}

bool moveEntrySimple(const std::string& src, const std::string& dst,
                     std::string& errOut, const ProgressCb& cb) {
    std::error_code ec;
    stdfs::rename(stdfs::path(src), stdfs::path(dst), ec);
    if (!ec) return true;
    if (!copyTreeCb(src, dst, errOut, cb)) return false;
    return removeEntry(src, errOut);
}

} // namespace fs
} // namespace xplore
