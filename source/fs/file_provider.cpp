#include "fs/file_provider.hpp"
#include <cstring>

namespace xplore {
namespace fs {

bool FileProvider::writeFileChunk(const std::string& path, uint64_t offset,
                                  const void* data, size_t size, bool truncate,
                                  std::string& errOut) {
    if (truncate && offset == 0)
        return writeFile(path, data, size, errOut);
    errOut = "partial write unsupported";
    return false;
}

bool FileProvider::copyFile(const std::string& src, const std::string& dst,
                            std::string& errOut, const ProviderProgressCb& cb) {
    FileStatInfo info;
    if (!statPath(src, info, errOut))
        return false;
    if (info.isDirectory) {
        errOut = "source is a directory";
        return false;
    }

    if (supportsPartialWrite()) {
        static constexpr size_t kChunk = 1024 * 1024;
        std::vector<char> buf(kChunk);
        uint64_t remaining = info.size;
        uint64_t offset = 0;

        if (remaining == 0)
            return writeFileChunk(dst, 0, nullptr, 0, true, errOut);

        while (remaining > 0) {
            size_t chunk = static_cast<size_t>(
                std::min<uint64_t>(remaining, static_cast<uint64_t>(buf.size())));
            if (!readFile(src, offset, chunk, buf.data(), errOut))
                return false;
            if (cb && !cb(dst)) {
                errOut = "interrupted";
                return false;
            }
            if (!writeFileChunk(dst, offset, buf.data(), chunk, offset == 0, errOut))
                return false;
            offset += chunk;
            remaining -= chunk;
        }
        return true;
    }

    if (info.size > 256ULL * 1024ULL * 1024ULL) {
        errOut = "destination provider does not support streaming writes (>256MB)";
        return false;
    }

    std::vector<char> fullBuf(static_cast<size_t>(info.size));
    if (info.size > 0) {
        if (!readFile(src, 0, static_cast<size_t>(info.size), fullBuf.data(), errOut))
            return false;
    }

    if (cb && !cb(dst)) {
        errOut = "interrupted";
        return false;
    }

    return writeFile(dst, fullBuf.data(), fullBuf.size(), errOut);
}

bool FileProvider::moveFile(const std::string& src, const std::string& dst,
                            std::string& errOut, const ProviderProgressCb& cb) {
    // Try rename first (same filesystem)
    if (renamePath(src, dst, errOut))
        return true;

    // Fallback: copy + delete
    errOut.clear();
    if (!copyFile(src, dst, errOut, cb))
        return false;
    return removeAll(src, errOut);
}

bool FileProvider::copyEntry(const std::string& src, const std::string& dst,
                             std::string& errOut, const ProviderProgressCb& cb) {
    FileStatInfo info;
    if (!statPath(src, info, errOut))
        return false;

    if (!info.isDirectory)
        return copyFile(src, dst, errOut, cb);

    // Directory: create dest, recurse
    if (!createDirectory(dst, errOut))
        return false;

    auto entries = listDir(src, errOut);
    if (!errOut.empty())
        return false;

    for (const auto& e : entries) {
        if (cb && !cb(e.name)) {
            errOut = "interrupted";
            return false;
        }
        std::string srcChild = src;
        if (!srcChild.empty() && srcChild.back() != '/')
            srcChild += '/';
        srcChild += e.name;

        std::string dstChild = dst;
        if (!dstChild.empty() && dstChild.back() != '/')
            dstChild += '/';
        dstChild += e.name;

        if (!copyEntry(srcChild, dstChild, errOut, cb))
            return false;
    }
    return true;
}

bool FileProvider::moveEntry(const std::string& src, const std::string& dst,
                             std::string& errOut, const ProviderProgressCb& cb) {
    // Try rename first
    if (renamePath(src, dst, errOut))
        return true;

    errOut.clear();
    if (!copyEntry(src, dst, errOut, cb))
        return false;
    return removeAll(src, errOut);
}

bool FileProvider::pathExists(const std::string& path) {
    FileStatInfo info;
    std::string err;
    return statPath(path, info, err) && info.exists;
}

bool FileProvider::isDirectory(const std::string& path) {
    FileStatInfo info;
    std::string err;
    return statPath(path, info, err) && info.isDirectory;
}

uint64_t FileProvider::fileSize(const std::string& path) {
    FileStatInfo info;
    std::string err;
    if (!statPath(path, info, err))
        return 0;
    return info.size;
}

} // namespace fs
} // namespace xplore
