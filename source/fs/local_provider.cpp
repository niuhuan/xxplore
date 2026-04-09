#include "fs/local_provider.hpp"
#include "fs/fs_api.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace xxplore {
namespace fs {

/// Convert provider-relative path to full sdmc: path.
static std::string toFull(const std::string& relPath) {
    return "sdmc:" + relPath;
}

std::vector<FileEntry> LocalFileProvider::listDir(const std::string& path,
                                                  std::string& errOut) {
    auto result = fs::listDir(toFull(path));
    return result;
}

bool LocalFileProvider::statPath(const std::string& path, FileStatInfo& out,
                                 std::string& errOut) {
    return fs::statPath(toFull(path), out);
}

bool LocalFileProvider::createDirectory(const std::string& path,
                                        std::string& errOut) {
    return fs::createDirectory(toFull(path), errOut);
}

bool LocalFileProvider::removeAll(const std::string& path, std::string& errOut) {
    return fs::removeAll(toFull(path), errOut);
}

bool LocalFileProvider::renamePath(const std::string& from, const std::string& to,
                                   std::string& errOut) {
    return fs::renamePath(toFull(from), toFull(to), errOut);
}

bool LocalFileProvider::readFile(const std::string& path, uint64_t offset,
                                 size_t size, void* outBuffer,
                                 std::string& errOut) {
    std::string full = toFull(path);
    FILE* f = fopen(full.c_str(), "rb");
    if (!f) {
        errOut = std::string("open: ") + strerror(errno);
        return false;
    }
    if (offset > 0) {
        if (fseeko(f, static_cast<off_t>(offset), SEEK_SET) != 0) {
            errOut = std::string("seek: ") + strerror(errno);
            fclose(f);
            return false;
        }
    }
    size_t nread = fread(outBuffer, 1, size, f);
    if (nread != size && ferror(f)) {
        errOut = std::string("read: ") + strerror(errno);
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

bool LocalFileProvider::writeFile(const std::string& path, const void* data,
                                  size_t size, std::string& errOut) {
    std::string full = toFull(path);
    FILE* f = fopen(full.c_str(), "wb");
    if (!f) {
        errOut = std::string("open: ") + strerror(errno);
        return false;
    }
    if (size > 0 && fwrite(data, 1, size, f) != size) {
        errOut = std::string("write: ") + strerror(errno);
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

bool LocalFileProvider::writeFileChunk(const std::string& path, uint64_t offset,
                                       const void* data, size_t size, bool truncate,
                                       std::string& errOut) {
    std::string full = toFull(path);
    FILE* f = fopen(full.c_str(), truncate ? "wb" : "r+b");
    if (!f && !truncate)
        f = fopen(full.c_str(), "w+b");
    if (!f) {
        errOut = std::string("open: ") + strerror(errno);
        return false;
    }
    if (offset > 0) {
        if (fseeko(f, static_cast<off_t>(offset), SEEK_SET) != 0) {
            errOut = std::string("seek: ") + strerror(errno);
            fclose(f);
            return false;
        }
    }
    if (size > 0 && fwrite(data, 1, size, f) != size) {
        errOut = std::string("write: ") + strerror(errno);
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

bool LocalFileProvider::copyFile(const std::string& src, const std::string& dst,
                                 std::string& errOut,
                                 const ProviderProgressCb& cb) {
    ProgressCb adapted = nullptr;
    if (cb) {
        adapted = [&cb](const std::string& f) -> bool { return cb(f); };
    }
    return fs::copyEntrySimple(toFull(src), toFull(dst), errOut, adapted);
}

bool LocalFileProvider::moveFile(const std::string& src, const std::string& dst,
                                 std::string& errOut,
                                 const ProviderProgressCb& cb) {
    ProgressCb adapted = nullptr;
    if (cb) {
        adapted = [&cb](const std::string& f) -> bool { return cb(f); };
    }
    return fs::moveEntrySimple(toFull(src), toFull(dst), errOut, adapted);
}

bool LocalFileProvider::copyEntry(const std::string& src, const std::string& dst,
                                  std::string& errOut,
                                  const ProviderProgressCb& cb) {
    ProgressCb adapted = nullptr;
    if (cb) {
        adapted = [&cb](const std::string& f) -> bool { return cb(f); };
    }
    return fs::copyEntrySimple(toFull(src), toFull(dst), errOut, adapted);
}

bool LocalFileProvider::moveEntry(const std::string& src, const std::string& dst,
                                  std::string& errOut,
                                  const ProviderProgressCb& cb) {
    ProgressCb adapted = nullptr;
    if (cb) {
        adapted = [&cb](const std::string& f) -> bool { return cb(f); };
    }
    return fs::moveEntrySimple(toFull(src), toFull(dst), errOut, adapted);
}

} // namespace fs
} // namespace xxplore
