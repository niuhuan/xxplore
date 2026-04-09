#include "fs/provider_manager.hpp"
#include "fs/local_provider.hpp"
#include "fs/webdav_provider.hpp"
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace xplore {
namespace fs {

namespace {

constexpr size_t kTransferChunkSize = 128 * 1024;
constexpr size_t kTransferBufferCount = 20;

std::string pathBaseName(const std::string& fullPath) {
    std::size_t slash = fullPath.find_last_of('/');
    if (slash == std::string::npos)
        return fullPath;
    if (slash + 1 >= fullPath.size())
        return {};
    return fullPath.substr(slash + 1);
}

bool validateDestinationName(const std::string& dstPath, std::string& errOut) {
    if (ProviderManager::extractPrefix(dstPath) != "sdmc:")
        return true;

    std::string name = pathBaseName(dstPath);
    if (name.empty())
        return true;
    if (isValidEnglishFileName(name))
        return true;

    errOut = "destination filename is not ASCII";
    return false;
}

void markIgnoredError(TransferResult& result, TransferOperation operation,
                      const std::string& message) {
    result.ignoredErrors = true;
    result.lastError = message;
    switch (operation) {
    case TransferOperation::Copy: result.copyHadErrors = true; break;
    case TransferOperation::Move: result.moveHadErrors = true; break;
    case TransferOperation::Delete: result.deleteHadErrors = true; break;
    }
}

bool reportProgress(const TransferCallbacks& callbacks, TransferResult& result,
                    const TransferProgress& progress) {
    if (!callbacks.onProgress)
        return true;
    if (callbacks.onProgress(progress))
        return true;
    result.interrupted = true;
    result.lastError = "interrupted";
    return false;
}

bool handleTransferError(const TransferCallbacks& callbacks, TransferResult& result,
                         bool& ignoreAll, const TransferError& error) {
    result.lastError = error.message;
    if (ignoreAll) {
        markIgnoredError(result, error.operation, error.message);
        return true;
    }

    TransferDecision decision = TransferDecision::Abort;
    if (callbacks.onError)
        decision = callbacks.onError(error);

    switch (decision) {
    case TransferDecision::Ignore:
        markIgnoredError(result, error.operation, error.message);
        return true;
    case TransferDecision::IgnoreAll:
        ignoreAll = true;
        markIgnoredError(result, error.operation, error.message);
        return true;
    case TransferDecision::Abort:
    default:
        result.aborted = true;
        return false;
    }
}

class BufferedFileReader {
public:
    BufferedFileReader(FileProvider* srcProv, std::string srcRel, uint64_t totalBytes)
        : srcProv_(srcProv), srcRel_(std::move(srcRel)), totalBytes_(totalBytes) {
        threaded_ = totalBytes_ > kTransferChunkSize && kTransferBufferCount > 1;
    }

    ~BufferedFileReader() { stop(); }

    bool start(std::string& errOut) {
        if (!srcProv_) {
            errOut = "missing source provider";
            return false;
        }
        stop();
        nextOffset_ = 0;
        bytesScheduled_ = 0;
        producerDone_ = false;
        stopRequested_ = false;
        producerError_.clear();
        queue_.clear();
        directOffset_ = 0;
        if (!threaded_)
            return true;
        worker_ = std::thread([this]() { workerLoop(); });
        return true;
    }

    bool readNext(void* outBuffer, size_t size, std::string& errOut) {
        if (!threaded_) {
            bool ok = srcProv_->readFile(srcRel_, directOffset_, size, outBuffer, errOut);
            if (ok)
                directOffset_ += size;
            return ok;
        }

        Chunk chunk;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cvNotEmpty_.wait(lock, [this]() {
                return stopRequested_ || !queue_.empty() || producerDone_ || !producerError_.empty();
            });

            if (!producerError_.empty()) {
                errOut = producerError_;
                return false;
            }
            if (queue_.empty()) {
                errOut = "prefetch ended unexpectedly";
                return false;
            }
            chunk = std::move(queue_.front());
            queue_.pop_front();
            cvNotFull_.notify_one();
        }

        if (!chunk.error.empty()) {
            errOut = chunk.error;
            return false;
        }
        if (chunk.data.size() != size) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "chunk size mismatch: expected %zu got %zu",
                          size, chunk.data.size());
            errOut = buf;
            return false;
        }

        std::memcpy(outBuffer, chunk.data.data(), size);
        return true;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopRequested_ = true;
            cvNotEmpty_.notify_all();
            cvNotFull_.notify_all();
        }
        if (worker_.joinable())
            worker_.join();
    }

private:
    struct Chunk {
        std::vector<unsigned char> data;
        std::string error;
    };

    void workerLoop() {
        for (;;) {
            uint64_t offset = 0;
            size_t size = 0;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cvNotFull_.wait(lock, [this]() {
                    return stopRequested_ || queue_.size() < kTransferBufferCount;
                });
                if (stopRequested_)
                    break;
                if (bytesScheduled_ >= totalBytes_) {
                    producerDone_ = true;
                    cvNotEmpty_.notify_all();
                    break;
                }

                offset = nextOffset_;
                uint64_t remaining = totalBytes_ - bytesScheduled_;
                size = static_cast<size_t>(std::min<uint64_t>(remaining, kTransferChunkSize));
                nextOffset_ += size;
                bytesScheduled_ += size;
            }

            Chunk chunk;
            chunk.data.resize(size);
            std::string err;
            if (!srcProv_->readFile(srcRel_, offset, size, chunk.data.data(), err))
                chunk.error = err.empty() ? "read failed" : err;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!chunk.error.empty()) {
                    producerError_ = chunk.error;
                    producerDone_ = true;
                    cvNotEmpty_.notify_all();
                    break;
                }
                queue_.push_back(std::move(chunk));
                cvNotEmpty_.notify_one();
            }
        }
    }

    FileProvider* srcProv_ = nullptr;
    std::string srcRel_;
    uint64_t totalBytes_ = 0;
    bool threaded_ = false;
    uint64_t directOffset_ = 0;

    uint64_t nextOffset_ = 0;
    uint64_t bytesScheduled_ = 0;
    bool producerDone_ = false;
    bool stopRequested_ = false;
    std::string producerError_;
    std::deque<Chunk> queue_;
    std::mutex mutex_;
    std::condition_variable cvNotEmpty_;
    std::condition_variable cvNotFull_;
    std::thread worker_;
};

struct TransferRoot {
    std::string srcPath;
    std::string dstPath;
    uint64_t    totalBytes = 0;
};

bool tryStat(ProviderManager& provMgr, const std::string& fullPath, FileStatInfo& out,
             std::string& errOut) {
    out = {};
    if (!provMgr.statPath(fullPath, out, errOut))
        return false;
    return true;
}

bool scanEntryBytes(ProviderManager& provMgr, const std::string& fullPath,
                    uint64_t& totalBytes, std::string& errOut) {
    FileStatInfo info;
    if (!tryStat(provMgr, fullPath, info, errOut))
        return false;
    if (!info.exists) {
        errOut = "source does not exist";
        return false;
    }
    if (!info.isDirectory) {
        totalBytes += info.size;
        return true;
    }

    auto entries = provMgr.listDir(fullPath, errOut);
    if (!errOut.empty())
        return false;
    for (const auto& entry : entries) {
        std::string child = joinPath(fullPath, entry.name);
        if (!scanEntryBytes(provMgr, child, totalBytes, errOut))
            return false;
    }
    return true;
}

bool copyFileBuffered(ProviderManager& provMgr, FileProvider* srcProv, const std::string& srcRel,
                      const std::string& srcFull, FileProvider* dstProv,
                      const std::string& dstRel, const std::string& dstFull,
                      uint64_t fileSize, TransferOperation operation,
                      const TransferCallbacks& callbacks, TransferResult& result,
                      uint64_t& overallDone, uint64_t overallTotal) {
    std::string err;
    if (!validateDestinationName(dstFull, err)) {
        result.lastError = err;
        return false;
    }

    auto emitProgress = [&](uint64_t currentBytes) -> bool {
        TransferProgress progress;
        progress.operation = operation;
        progress.currentPath = srcFull;
        progress.targetPath = dstFull;
        progress.currentBytes = currentBytes;
        progress.currentTotalBytes = fileSize;
        progress.overallBytes = overallDone + currentBytes;
        progress.overallTotalBytes = overallTotal;
        return reportProgress(callbacks, result, progress);
    };

    if (!emitProgress(0))
        return false;

    auto cleanupPartial = [&]() {
        std::string cleanupErr;
        provMgr.removeAll(dstFull, cleanupErr);
    };

    if (dstProv->kind() == ProviderKind::WebDav) {
        auto* webdavDst = static_cast<WebDavProvider*>(dstProv);
        BufferedFileReader reader(srcProv, srcRel, fileSize);
        if (!reader.start(err)) {
            result.lastError = err;
            return false;
        }
        uint64_t nextOffset = 0;
        bool ok = webdavDst->uploadFromStream(
            dstRel, fileSize,
            [&](void* outBuffer, size_t size, uint64_t offset, std::string& readErr) -> bool {
                if (offset != nextOffset) {
                    readErr = "unexpected PUT stream offset";
                    return false;
                }
                if (size == 0)
                    return true;
                bool readOk = reader.readNext(outBuffer, size, readErr);
                if (readOk)
                    nextOffset += size;
                return readOk;
            },
            err,
            [&](uint64_t sentBytes) -> bool {
                return emitProgress(sentBytes);
            });
        reader.stop();
        if (!ok && err != "interrupted")
            cleanupPartial();
        result.lastError = err;
        if (ok)
            overallDone += fileSize;
        return ok;
    }

    if (dstProv->supportsPartialWrite()) {
        if (fileSize == 0) {
            bool ok = dstProv->writeFileChunk(dstRel, 0, nullptr, 0, true, err);
            result.lastError = err;
            if (ok)
                overallDone += fileSize;
            return ok;
        }

        BufferedFileReader reader(srcProv, srcRel, fileSize);
        if (!reader.start(err)) {
            result.lastError = err;
            return false;
        }
        std::vector<char> buf(kTransferChunkSize);
        uint64_t offset = 0;
        while (offset < fileSize) {
            size_t chunk = static_cast<size_t>(
                std::min<uint64_t>(fileSize - offset, static_cast<uint64_t>(buf.size())));
            if (!reader.readNext(buf.data(), chunk, err)) {
                reader.stop();
                result.lastError = err;
                cleanupPartial();
                return false;
            }
            if (!dstProv->writeFileChunk(dstRel, offset, buf.data(), chunk, offset == 0, err)) {
                reader.stop();
                result.lastError = err;
                cleanupPartial();
                return false;
            }
            offset += chunk;
            if (!emitProgress(offset)) {
                reader.stop();
                cleanupPartial();
                return false;
            }
        }
        reader.stop();
        overallDone += fileSize;
        return true;
    }

    if (fileSize > 256ULL * 1024ULL * 1024ULL) {
        result.lastError = "destination provider does not support streaming writes (>256MB)";
        return false;
    }

    std::vector<char> fullBuf(static_cast<size_t>(fileSize));
    if (fileSize > 0 && !srcProv->readFile(srcRel, 0, static_cast<size_t>(fileSize),
                                           fullBuf.data(), err)) {
        result.lastError = err;
        return false;
    }
    if (!dstProv->writeFile(dstRel, fullBuf.data(), fullBuf.size(), err)) {
        result.lastError = err;
        cleanupPartial();
        return false;
    }
    if (!emitProgress(fileSize)) {
        cleanupPartial();
        return false;
    }
    overallDone += fileSize;
    return true;
}

bool deletePathRecursive(ProviderManager& provMgr, const std::string& fullPath,
                         TransferOperation operation, const TransferCallbacks& callbacks,
                         TransferResult& result, bool& ignoreAll) {
    FileStatInfo info;
    std::string err;
    if (!tryStat(provMgr, fullPath, info, err)) {
        TransferError error{operation, fullPath, {}, err};
        return handleTransferError(callbacks, result, ignoreAll, error);
    }
    if (!info.exists)
        return true;

    if (info.isDirectory) {
        auto entries = provMgr.listDir(fullPath, err);
        if (!err.empty()) {
            TransferError error{operation, fullPath, {}, err};
            return handleTransferError(callbacks, result, ignoreAll, error);
        }
        for (const auto& entry : entries) {
            std::string child = joinPath(fullPath, entry.name);
            if (!deletePathRecursive(provMgr, child, operation, callbacks, result, ignoreAll))
                return false;
        }
    }

    TransferProgress progress;
    progress.operation = operation;
    progress.currentPath = fullPath;
    if (!reportProgress(callbacks, result, progress))
        return false;

    if (!provMgr.removeAll(fullPath, err)) {
        TransferError error{operation, fullPath, {}, err};
        return handleTransferError(callbacks, result, ignoreAll, error);
    }
    return true;
}

bool transferEntryRecursive(ProviderManager& provMgr, const std::string& srcFull,
                            const std::string& dstFull, TransferOperation operation,
                            TransferStrategy strategy, const TransferCallbacks& callbacks,
                            TransferResult& result, bool& ignoreAll, uint64_t& overallDone,
                            uint64_t overallTotal);

bool transferDirectoryRecursive(ProviderManager& provMgr, const std::string& srcFull,
                                const std::string& dstFull, TransferOperation operation,
                                TransferStrategy strategy, const TransferCallbacks& callbacks,
                                TransferResult& result, bool& ignoreAll, uint64_t& overallDone,
                                uint64_t overallTotal) {
    FileStatInfo dstInfo;
    std::string err;
    bool dstExists = false;
    bool dstIsDir = false;
    if (provMgr.pathExists(dstFull)) {
        if (!tryStat(provMgr, dstFull, dstInfo, err)) {
            TransferError error{operation, srcFull, dstFull, err};
            return handleTransferError(callbacks, result, ignoreAll, error);
        }
        dstExists = dstInfo.exists;
        dstIsDir = dstInfo.isDirectory;
    }

    if (dstExists && !dstIsDir) {
        if (strategy == TransferStrategy::Simple) {
            TransferError error{operation, srcFull, dstFull, "destination already exists"};
            return handleTransferError(callbacks, result, ignoreAll, error);
        }
        if (!deletePathRecursive(provMgr, dstFull, operation, callbacks, result, ignoreAll))
            return false;
        dstExists = false;
    }

    if (!dstExists) {
        if (!validateDestinationName(dstFull, err)) {
            TransferError error{operation, srcFull, dstFull, err};
            return handleTransferError(callbacks, result, ignoreAll, error);
        }
        if (!provMgr.createDirectory(dstFull, err)) {
            TransferError error{operation, srcFull, dstFull, err};
            return handleTransferError(callbacks, result, ignoreAll, error);
        }
    }

    auto entries = provMgr.listDir(srcFull, err);
    if (!err.empty()) {
        TransferError error{operation, srcFull, dstFull, err};
        return handleTransferError(callbacks, result, ignoreAll, error);
    }
    for (const auto& entry : entries) {
        std::string srcChild = joinPath(srcFull, entry.name);
        std::string dstChild = joinPath(dstFull, entry.name);
        if (!transferEntryRecursive(provMgr, srcChild, dstChild, operation, strategy,
                                    callbacks, result, ignoreAll, overallDone, overallTotal))
            return false;
    }
    return true;
}

bool transferEntryRecursive(ProviderManager& provMgr, const std::string& srcFull,
                            const std::string& dstFull, TransferOperation operation,
                            TransferStrategy strategy, const TransferCallbacks& callbacks,
                            TransferResult& result, bool& ignoreAll, uint64_t& overallDone,
                            uint64_t overallTotal) {
    FileStatInfo srcInfo;
    std::string err;
    if (!tryStat(provMgr, srcFull, srcInfo, err)) {
        TransferError error{operation, srcFull, dstFull, err};
        return handleTransferError(callbacks, result, ignoreAll, error);
    }
    if (!srcInfo.exists) {
        TransferError error{operation, srcFull, dstFull, "source does not exist"};
        return handleTransferError(callbacks, result, ignoreAll, error);
    }

    if (srcInfo.isDirectory)
        return transferDirectoryRecursive(provMgr, srcFull, dstFull, operation, strategy,
                                          callbacks, result, ignoreAll, overallDone,
                                          overallTotal);

    if (provMgr.pathExists(dstFull)) {
        if (strategy == TransferStrategy::Simple) {
            TransferError error{operation, srcFull, dstFull, "destination already exists"};
            return handleTransferError(callbacks, result, ignoreAll, error);
        }
        if (!deletePathRecursive(provMgr, dstFull, operation, callbacks, result, ignoreAll))
            return false;
    }

    std::string srcRel;
    std::string dstRel;
    FileProvider* srcProv = provMgr.resolveProvider(srcFull, srcRel);
    FileProvider* dstProv = provMgr.resolveProvider(dstFull, dstRel);
    if (!srcProv || !dstProv) {
        TransferError error{operation, srcFull, dstFull, "unknown provider"};
        return handleTransferError(callbacks, result, ignoreAll, error);
    }

    if (!copyFileBuffered(provMgr, srcProv, srcRel, srcFull, dstProv, dstRel, dstFull,
                          srcInfo.size, operation, callbacks, result, overallDone,
                          overallTotal)) {
        if (result.interrupted || result.aborted)
            return false;
        TransferError error{operation, srcFull, dstFull,
                            result.lastError.empty() ? "copy failed" : result.lastError};
        return handleTransferError(callbacks, result, ignoreAll, error);
    }
    return true;
}

} // namespace

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

FileProvider* ProviderManager::findProviderByDisplayPrefix(const std::string& displayPrefix) {
    for (auto& p : providers_)
        if (p->displayPrefix() == displayPrefix)
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
    for (const auto& p : providers_) {
        if (p->displayPrefix() != prefix)
            continue;
        return p->kind() == ProviderKind::WebDav || p->kind() == ProviderKind::Smb;
    }
    return false;
}

TransferResult ProviderManager::transferEntries(const std::vector<TransferEntry>& entries,
                                                TransferOperation operation,
                                                TransferStrategy strategy,
                                                const TransferCallbacks& callbacks) {
    TransferResult result;
    if (operation != TransferOperation::Copy && operation != TransferOperation::Move) {
        result.aborted = true;
        result.lastError = "invalid transfer operation";
        return result;
    }

    std::vector<TransferRoot> roots;
    roots.reserve(entries.size());
    uint64_t overallTotal = 0;
    for (const auto& entry : entries) {
        TransferRoot root;
        root.srcPath = entry.srcPath;
        root.dstPath = entry.dstPath;
        std::string err;
        if (!scanEntryBytes(*this, entry.srcPath, root.totalBytes, err)) {
            result.aborted = true;
            result.lastError = err;
            return result;
        }
        overallTotal += root.totalBytes;
        roots.push_back(std::move(root));
    }

    bool ignoreAll = false;
    uint64_t overallDone = 0;
    for (const auto& root : roots) {
        if (result.interrupted || result.aborted)
            break;

        if (operation == TransferOperation::Move &&
            strategy == TransferStrategy::Simple &&
            isSameProvider(root.srcPath, root.dstPath) &&
            !pathExists(root.dstPath)) {
            std::string renameErr;
            if (renamePath(root.srcPath, root.dstPath, renameErr)) {
                overallDone += root.totalBytes;
                TransferProgress progress;
                progress.operation = operation;
                progress.currentPath = root.srcPath;
                progress.targetPath = root.dstPath;
                progress.currentBytes = root.totalBytes;
                progress.currentTotalBytes = root.totalBytes;
                progress.overallBytes = overallDone;
                progress.overallTotalBytes = overallTotal;
                if (!reportProgress(callbacks, result, progress))
                    break;
                continue;
            }
        }

        if (!transferEntryRecursive(*this, root.srcPath, root.dstPath, operation, strategy,
                                    callbacks, result, ignoreAll, overallDone,
                                    overallTotal)) {
            if (result.interrupted || result.aborted)
                break;
        }

        if (operation == TransferOperation::Move &&
            !result.interrupted && !result.aborted) {
            if (!deletePathRecursive(*this, root.srcPath, TransferOperation::Move, callbacks,
                                     result, ignoreAll))
                break;
        }
    }

    return result;
}

TransferResult ProviderManager::deleteEntries(const std::vector<std::string>& paths,
                                              const TransferCallbacks& callbacks) {
    TransferResult result;
    bool ignoreAll = false;
    for (const auto& path : paths) {
        if (!deletePathRecursive(*this, path, TransferOperation::Delete, callbacks,
                                 result, ignoreAll)) {
            break;
        }
    }
    return result;
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
        if (cb && !cb(srcRel)) {
            errOut = "interrupted";
            return false;
        }

        if (dstProv->kind() == ProviderKind::WebDav) {
            auto* webdavDst = static_cast<WebDavProvider*>(dstProv);
            return webdavDst->uploadFromProvider(srcProv, srcRel, dstRel, srcInfo.size,
                                                 errOut, cb);
        }

        if (dstProv->supportsPartialWrite()) {
            static constexpr size_t kChunk = 1024 * 1024;
            std::vector<char> buf(kChunk);
            uint64_t remaining = srcInfo.size;
            uint64_t offset = 0;

            if (remaining == 0)
                return dstProv->writeFileChunk(dstRel, 0, nullptr, 0, true, errOut);

            while (remaining > 0) {
                size_t chunk = static_cast<size_t>(
                    std::min<uint64_t>(remaining, static_cast<uint64_t>(buf.size())));
                if (!srcProv->readFile(srcRel, offset, chunk, buf.data(), errOut))
                    return false;
                if (cb && !cb(dstRel)) {
                    errOut = "interrupted";
                    return false;
                }
                if (!dstProv->writeFileChunk(dstRel, offset, buf.data(), chunk,
                                             offset == 0, errOut)) {
                    return false;
                }
                offset += chunk;
                remaining -= chunk;
            }
            return true;
        }

        if (srcInfo.size > 256ULL * 1024 * 1024) {
            errOut = "destination provider does not support streaming writes (>256MB)";
            return false;
        }

        std::vector<char> buf(static_cast<size_t>(srcInfo.size));
        if (srcInfo.size > 0) {
            if (!srcProv->readFile(srcRel, 0, static_cast<size_t>(srcInfo.size),
                                   buf.data(), errOut))
                return false;
        }
        return dstProv->writeFile(dstRel, buf.data(), buf.size(), errOut);
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
