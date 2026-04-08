#include "fs/provider_manager.hpp"
#include "fs/local_provider.hpp"
#include <algorithm>
#include <cstdio>

namespace xplore {
namespace fs {

ProviderManager::ProviderManager() {
    // Register local provider by default
    registerProvider(std::make_shared<LocalFileProvider>());
}

void ProviderManager::registerProvider(std::shared_ptr<FileProvider> provider) {
    if (!provider)
        return;

    // Remove existing with same id
    auto id = provider->providerId();
    providers_.erase(
        std::remove_if(providers_.begin(), providers_.end(),
                       [&id](const auto& p) { return p->providerId() == id; }),
        providers_.end());
    providers_.push_back(std::move(provider));
}

void ProviderManager::removeProvider(const std::string& providerId) {
    providers_.erase(
        std::remove_if(providers_.begin(), providers_.end(),
                       [&providerId](const auto& p) { return p->providerId() == providerId; }),
        providers_.end());
}

FileProvider* ProviderManager::findProvider(const std::string& providerId) {
    for (auto& p : providers_)
        if (p->providerId() == providerId)
            return p.get();
    return nullptr;
}

std::string ProviderManager::extractPrefix(const std::string& fullPath) {
    // Look for ":" that marks end of provider prefix
    // e.g. "sdmc:/foo" → "sdmc:", "webdav-abc123:/dir" → "webdav-abc123:"
    auto colonPos = fullPath.find(':');
    if (colonPos == std::string::npos || colonPos == 0)
        return {};
    return fullPath.substr(0, colonPos + 1);
}

FileProvider* ProviderManager::resolveProvider(const std::string& fullPath,
                                               std::string& outRelativePath) {
    if (isVirtualRoot(fullPath))
        return nullptr;

    std::string prefix = extractPrefix(fullPath);
    if (prefix.empty())
        return nullptr;

    for (auto& p : providers_) {
        if (p->displayPrefix() == prefix) {
            // relativePath: everything after "prefix/"
            // e.g. "sdmc:/foo/bar" → prefix="sdmc:", relative="/foo/bar"
            std::string rest = fullPath.substr(prefix.size());
            if (rest.empty())
                rest = "/";
            outRelativePath = rest;
            return p.get();
        }
    }
    return nullptr;
}

std::vector<FileEntry> ProviderManager::getRootEntries() const {
    std::vector<FileEntry> entries;
    for (const auto& p : providers_) {
        FileEntry e;
        e.name = p->displayPrefix();
        e.isDirectory = true;
        e.size = 0;
        entries.push_back(std::move(e));
    }
    return entries;
}

bool ProviderManager::pathAllowsSelection(const std::string& fullPath) const {
    if (isVirtualRoot(fullPath))
        return false;

    std::string prefix = extractPrefix(fullPath);
    if (prefix.empty())
        return false;

    for (const auto& p : providers_) {
        if (p->displayPrefix() == prefix)
            return p->allowsSelection() && !p->isReadOnly();
    }
    return false;
}

// --- Delegated operations ---

std::vector<FileEntry> ProviderManager::listDir(const std::string& fullPath,
                                                std::string& errOut) {
    if (isVirtualRoot(fullPath))
        return getRootEntries();

    std::string relPath;
    FileProvider* prov = resolveProvider(fullPath, relPath);
    if (!prov) {
        errOut = "unknown provider for path: " + fullPath;
        return {};
    }
    return prov->listDir(relPath, errOut);
}

bool ProviderManager::statPath(const std::string& fullPath, FileStatInfo& out,
                               std::string& errOut) {
    std::string relPath;
    FileProvider* prov = resolveProvider(fullPath, relPath);
    if (!prov) {
        errOut = "unknown provider";
        return false;
    }
    return prov->statPath(relPath, out, errOut);
}

bool ProviderManager::createDirectory(const std::string& fullPath,
                                      std::string& errOut) {
    std::string relPath;
    FileProvider* prov = resolveProvider(fullPath, relPath);
    if (!prov) {
        errOut = "unknown provider";
        return false;
    }
    return prov->createDirectory(relPath, errOut);
}

bool ProviderManager::removeAll(const std::string& fullPath, std::string& errOut) {
    std::string relPath;
    FileProvider* prov = resolveProvider(fullPath, relPath);
    if (!prov) {
        errOut = "unknown provider";
        return false;
    }
    return prov->removeAll(relPath, errOut);
}

bool ProviderManager::renamePath(const std::string& from, const std::string& to,
                                 std::string& errOut) {
    std::string relFrom, relTo;
    FileProvider* provFrom = resolveProvider(from, relFrom);
    FileProvider* provTo   = resolveProvider(to, relTo);
    if (!provFrom || !provTo) {
        errOut = "unknown provider";
        return false;
    }
    if (provFrom != provTo) {
        errOut = "cannot rename across providers";
        return false;
    }
    return provFrom->renamePath(relFrom, relTo, errOut);
}

bool ProviderManager::readFile(const std::string& fullPath, uint64_t offset,
                               size_t size, void* outBuffer,
                               std::string& errOut) {
    std::string relPath;
    FileProvider* prov = resolveProvider(fullPath, relPath);
    if (!prov) {
        errOut = "unknown provider";
        return false;
    }
    return prov->readFile(relPath, offset, size, outBuffer, errOut);
}

bool ProviderManager::writeFile(const std::string& fullPath, const void* data,
                                size_t size, std::string& errOut) {
    std::string relPath;
    FileProvider* prov = resolveProvider(fullPath, relPath);
    if (!prov) {
        errOut = "unknown provider";
        return false;
    }
    return prov->writeFile(relPath, data, size, errOut);
}

bool ProviderManager::pathExists(const std::string& fullPath) {
    std::string relPath;
    FileProvider* prov = resolveProvider(fullPath, relPath);
    if (!prov)
        return false;
    return prov->pathExists(relPath);
}

bool ProviderManager::isDirectoryPath(const std::string& fullPath) {
    std::string relPath;
    FileProvider* prov = resolveProvider(fullPath, relPath);
    if (!prov)
        return false;
    return prov->isDirectory(relPath);
}

bool ProviderManager::isSameProvider(const std::string& pathA,
                                     const std::string& pathB) const {
    return extractPrefix(pathA) == extractPrefix(pathB);
}

bool ProviderManager::isNetworkPath(const std::string& fullPath) const {
    std::string prefix = extractPrefix(fullPath);
    if (prefix.empty())
        return false;
    // Local provider prefix is "sdmc:"
    return prefix != "sdmc:";
}

// --- Cross-provider copy ---

bool ProviderManager::crossProviderCopy(FileProvider* srcProv,
                                        const std::string& srcRel,
                                        FileProvider* dstProv,
                                        const std::string& dstRel,
                                        std::string& errOut,
                                        const ProviderProgressCb& cb) {
    FileStatInfo srcInfo;
    if (!srcProv->statPath(srcRel, srcInfo, errOut))
        return false;

    if (!srcInfo.isDirectory) {
        // File: stream copy in chunks
        if (cb && !cb(srcRel)) {
            errOut = "interrupted";
            return false;
        }

        // For files up to 256MB, read all at once; larger files use chunked writes
        if (srcInfo.size <= 256ULL * 1024 * 1024) {
            std::vector<char> buf(static_cast<size_t>(srcInfo.size));
            if (srcInfo.size > 0) {
                if (!srcProv->readFile(srcRel, 0, static_cast<size_t>(srcInfo.size),
                                       buf.data(), errOut))
                    return false;
            }
            return dstProv->writeFile(dstRel, buf.data(), buf.size(), errOut);
        } else {
            errOut = "file too large for cross-provider copy (>256MB)";
            return false;
        }
    }

    // Directory: create on dst, recurse
    if (!dstProv->createDirectory(dstRel, errOut))
        return false;

    auto entries = srcProv->listDir(srcRel, errOut);
    if (!errOut.empty())
        return false;

    for (const auto& e : entries) {
        if (cb && !cb(e.name)) {
            errOut = "interrupted";
            return false;
        }

        std::string srcChild = srcRel;
        if (!srcChild.empty() && srcChild.back() != '/')
            srcChild += '/';
        srcChild += e.name;

        std::string dstChild = dstRel;
        if (!dstChild.empty() && dstChild.back() != '/')
            dstChild += '/';
        dstChild += e.name;

        if (!crossProviderCopy(srcProv, srcChild, dstProv, dstChild, errOut, cb))
            return false;
    }
    return true;
}

// --- Copy/Move with strategy ---

bool ProviderManager::copyEntry(const std::string& src, const std::string& dst,
                                std::string& errOut,
                                const ProviderProgressCb& cb) {
    return copyEntrySimple(src, dst, errOut, cb);
}

bool ProviderManager::moveEntry(const std::string& src, const std::string& dst,
                                std::string& errOut,
                                const ProviderProgressCb& cb) {
    return moveEntrySimple(src, dst, errOut, cb);
}

bool ProviderManager::copyEntrySimple(const std::string& src, const std::string& dst,
                                      std::string& errOut,
                                      const ProviderProgressCb& cb) {
    if (isSameProvider(src, dst)) {
        std::string relSrc, relDst;
        FileProvider* prov = resolveProvider(src, relSrc);
        resolveProvider(dst, relDst);
        if (prov)
            return prov->copyEntry(relSrc, relDst, errOut, cb);
        errOut = "unknown provider";
        return false;
    }

    // Cross-provider
    std::string relSrc, relDst;
    FileProvider* srcProv = resolveProvider(src, relSrc);
    FileProvider* dstProv = resolveProvider(dst, relDst);
    if (!srcProv || !dstProv) {
        errOut = "unknown provider";
        return false;
    }
    return crossProviderCopy(srcProv, relSrc, dstProv, relDst, errOut, cb);
}

bool ProviderManager::moveEntrySimple(const std::string& src, const std::string& dst,
                                      std::string& errOut,
                                      const ProviderProgressCb& cb) {
    if (isSameProvider(src, dst)) {
        std::string relSrc, relDst;
        FileProvider* prov = resolveProvider(src, relSrc);
        resolveProvider(dst, relDst);
        if (prov)
            return prov->moveEntry(relSrc, relDst, errOut, cb);
        errOut = "unknown provider";
        return false;
    }

    // Cross-provider: copy then delete source
    if (!copyEntrySimple(src, dst, errOut, cb))
        return false;
    return removeAll(src, errOut);
}

bool ProviderManager::copyEntryOverwrite(const std::string& src, const std::string& dst,
                                         std::string& errOut,
                                         const ProviderProgressCb& cb) {
    if (isSameProvider(src, dst)) {
        // For local, use the optimized fs_ops functions
        std::string relSrc, relDst;
        FileProvider* prov = resolveProvider(src, relSrc);
        resolveProvider(dst, relDst);
        if (prov && prov->providerId() == "local") {
            ProgressCb adapted = nullptr;
            if (cb) adapted = [&cb](const std::string& f) -> bool { return cb(f); };
            return fs::copyEntryOverwrite(src, dst, errOut, adapted);
        }
    }
    // Generic: remove dst if exists, then copy
    if (pathExists(dst))
        removeAll(dst, errOut);
    errOut.clear();
    return copyEntrySimple(src, dst, errOut, cb);
}

bool ProviderManager::copyEntryMerge(const std::string& src, const std::string& dst,
                                     std::string& errOut,
                                     const ProviderProgressCb& cb) {
    if (isSameProvider(src, dst)) {
        std::string relSrc, relDst;
        FileProvider* prov = resolveProvider(src, relSrc);
        resolveProvider(dst, relDst);
        if (prov && prov->providerId() == "local") {
            ProgressCb adapted = nullptr;
            if (cb) adapted = [&cb](const std::string& f) -> bool { return cb(f); };
            return fs::copyEntryMerge(src, dst, errOut, adapted);
        }
    }
    // Generic merge: for non-local, just do simple copy (overwrite leaf files)
    // A full merge would need to check dst entries — keep it simple for v1
    return copyEntrySimple(src, dst, errOut, cb);
}

bool ProviderManager::moveEntryOverwrite(const std::string& src, const std::string& dst,
                                         std::string& errOut,
                                         const ProviderProgressCb& cb) {
    if (isSameProvider(src, dst)) {
        std::string relSrc, relDst;
        FileProvider* prov = resolveProvider(src, relSrc);
        resolveProvider(dst, relDst);
        if (prov && prov->providerId() == "local") {
            ProgressCb adapted = nullptr;
            if (cb) adapted = [&cb](const std::string& f) -> bool { return cb(f); };
            return fs::moveEntryOverwrite(src, dst, errOut, adapted);
        }
    }
    if (pathExists(dst))
        removeAll(dst, errOut);
    errOut.clear();
    return moveEntrySimple(src, dst, errOut, cb);
}

bool ProviderManager::moveEntryMerge(const std::string& src, const std::string& dst,
                                     std::string& errOut,
                                     const ProviderProgressCb& cb) {
    if (isSameProvider(src, dst)) {
        std::string relSrc, relDst;
        FileProvider* prov = resolveProvider(src, relSrc);
        resolveProvider(dst, relDst);
        if (prov && prov->providerId() == "local") {
            ProgressCb adapted = nullptr;
            if (cb) adapted = [&cb](const std::string& f) -> bool { return cb(f); };
            return fs::moveEntryMerge(src, dst, errOut, adapted);
        }
    }
    return moveEntrySimple(src, dst, errOut, cb);
}

} // namespace fs
} // namespace xplore
