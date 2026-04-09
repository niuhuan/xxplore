#include "fs/usb_mount_provider.hpp"
#include "fs/fs_api.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace xxplore::fs {

std::string UsbMountProvider::toFull(const std::string& relPath) const {
    return mountPrefix_ + relPath;
}

std::vector<FileEntry> UsbMountProvider::listDir(const std::string& path,
                                                 std::string& errOut) {
    (void)errOut;
    return fs::listDir(toFull(path));
}

bool UsbMountProvider::statPath(const std::string& path, FileStatInfo& out,
                                std::string& errOut) {
    (void)errOut;
    return fs::statPath(toFull(path), out);
}

bool UsbMountProvider::createDirectory(const std::string& path, std::string& errOut) {
    return fs::createDirectory(toFull(path), errOut);
}

bool UsbMountProvider::removeAll(const std::string& path, std::string& errOut) {
    return fs::removeAll(toFull(path), errOut);
}

bool UsbMountProvider::renamePath(const std::string& from, const std::string& to,
                                  std::string& errOut) {
    return fs::renamePath(toFull(from), toFull(to), errOut);
}

bool UsbMountProvider::readFile(const std::string& path, uint64_t offset,
                                size_t size, void* outBuffer, std::string& errOut) {
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

bool UsbMountProvider::writeFile(const std::string& path, const void* data,
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

bool UsbMountProvider::writeFileChunk(const std::string& path, uint64_t offset,
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

bool UsbMountProvider::copyFile(const std::string& src, const std::string& dst,
                                std::string& errOut, const ProviderProgressCb& cb) {
    ProgressCb adapted = nullptr;
    if (cb)
        adapted = [&cb](const std::string& f) -> bool { return cb(f); };
    return fs::copyEntrySimple(toFull(src), toFull(dst), errOut, adapted);
}

bool UsbMountProvider::moveFile(const std::string& src, const std::string& dst,
                                std::string& errOut, const ProviderProgressCb& cb) {
    ProgressCb adapted = nullptr;
    if (cb)
        adapted = [&cb](const std::string& f) -> bool { return cb(f); };
    return fs::moveEntrySimple(toFull(src), toFull(dst), errOut, adapted);
}

bool UsbMountProvider::copyEntry(const std::string& src, const std::string& dst,
                                 std::string& errOut, const ProviderProgressCb& cb) {
    ProgressCb adapted = nullptr;
    if (cb)
        adapted = [&cb](const std::string& f) -> bool { return cb(f); };
    return fs::copyEntrySimple(toFull(src), toFull(dst), errOut, adapted);
}

bool UsbMountProvider::moveEntry(const std::string& src, const std::string& dst,
                                 std::string& errOut, const ProviderProgressCb& cb) {
    ProgressCb adapted = nullptr;
    if (cb)
        adapted = [&cb](const std::string& f) -> bool { return cb(f); };
    return fs::moveEntrySimple(toFull(src), toFull(dst), errOut, adapted);
}

} // namespace xxplore::fs
