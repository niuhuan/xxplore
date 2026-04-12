#include "fs/zip_writer.hpp"

#include "fs/provider_manager.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <minizip/ioapi.h>
#include <minizip/zip.h>
#include <zlib.h>

namespace xxplore::fs {

namespace {

constexpr std::size_t kZipWriteBufferSize = 128 * 1024;
constexpr uLong kZipUtf8Flag = 1u << 11;

struct ArchiveWriteContext {
    FileProvider* provider = nullptr;
    std::string   relPath;
};

struct ArchiveWriteStream {
    ArchiveWriteContext* context = nullptr;
    uint64_t             offset = 0;
    uint64_t             size = 0;
    bool                 truncateNext = true;
    int                  error = 0;
};

bool isValidArchiveComponent(const std::string& value) {
    if (value.empty() || value == "." || value == "..")
        return false;
    return value.find('/') == std::string::npos &&
           value.find('\\') == std::string::npos;
}

bool isValidArchivePath(const std::string& value) {
    if (value.empty())
        return false;

    std::size_t start = 0;
    while (start < value.size()) {
        std::size_t slash = value.find('/', start);
        std::string part = slash == std::string::npos
                               ? value.substr(start)
                               : value.substr(start, slash - start);
        if (!isValidArchiveComponent(part))
            return false;
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return true;
}

std::string joinArchivePath(const std::string& parent, const std::string& name) {
    if (parent.empty())
        return name;
    return parent + "/" + name;
}

bool scanSourceBytes(ProviderManager& provMgr, const std::string& fullPath, uint64_t& totalBytes,
                     std::string& errOut) {
    FileStatInfo info;
    if (!provMgr.statPath(fullPath, info, errOut))
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
        if (!isValidArchiveComponent(entry.name)) {
            errOut = "invalid archive entry name";
            return false;
        }
        if (!scanSourceBytes(provMgr, joinPath(fullPath, entry.name), totalBytes, errOut))
            return false;
    }
    return true;
}

voidpf ZCALLBACK archiveWriteOpen64(voidpf opaque, const void* filename, int mode) {
    (void)filename;
    auto* context = static_cast<ArchiveWriteContext*>(opaque);
    if (!context || !context->provider || (mode & ZLIB_FILEFUNC_MODE_WRITE) == 0)
        return nullptr;

    auto* stream = new ArchiveWriteStream();
    stream->context = context;
    return stream;
}

uLong ZCALLBACK archiveWriteRead(voidpf opaque, voidpf stream, void* buf, uLong size) {
    (void)opaque;
    (void)stream;
    (void)buf;
    (void)size;
    return 0;
}

uLong ZCALLBACK archiveWriteWrite(voidpf opaque, voidpf streamPtr, const void* buf, uLong size) {
    (void)opaque;
    auto* stream = static_cast<ArchiveWriteStream*>(streamPtr);
    if (!stream || !stream->context || !stream->context->provider)
        return 0;

    std::string err;
    if (!stream->context->provider->writeFileChunk(stream->context->relPath, stream->offset, buf,
                                                   static_cast<std::size_t>(size),
                                                   stream->truncateNext, err)) {
        stream->error = 1;
        return 0;
    }

    stream->truncateNext = false;
    stream->offset += static_cast<uint64_t>(size);
    stream->size = std::max(stream->size, stream->offset);
    return size;
}

ZPOS64_T ZCALLBACK archiveWriteTell64(voidpf opaque, voidpf streamPtr) {
    (void)opaque;
    auto* stream = static_cast<ArchiveWriteStream*>(streamPtr);
    return stream ? static_cast<ZPOS64_T>(stream->offset) : 0;
}

long ZCALLBACK archiveWriteSeek64(voidpf opaque, voidpf streamPtr, ZPOS64_T offset, int origin) {
    (void)opaque;
    auto* stream = static_cast<ArchiveWriteStream*>(streamPtr);
    if (!stream)
        return -1;

    uint64_t next = 0;
    switch (origin) {
    case ZLIB_FILEFUNC_SEEK_SET:
        next = static_cast<uint64_t>(offset);
        break;
    case ZLIB_FILEFUNC_SEEK_CUR:
        next = stream->offset + static_cast<uint64_t>(offset);
        break;
    case ZLIB_FILEFUNC_SEEK_END:
        next = stream->size + static_cast<uint64_t>(offset);
        break;
    default:
        return -1;
    }

    stream->offset = next;
    return 0;
}

int ZCALLBACK archiveWriteClose(voidpf opaque, voidpf streamPtr) {
    (void)opaque;
    delete static_cast<ArchiveWriteStream*>(streamPtr);
    return 0;
}

int ZCALLBACK archiveWriteError(voidpf opaque, voidpf streamPtr) {
    (void)opaque;
    auto* stream = static_cast<ArchiveWriteStream*>(streamPtr);
    return stream ? stream->error : 0;
}

bool openZipEntry(zipFile archive, const std::string& archivePath, uint64_t fileSize,
                  bool isDirectory, std::string& errOut) {
    zip_fileinfo fileInfo {};
    const std::string name = isDirectory ? (archivePath + "/") : archivePath;
    const int method = isDirectory ? 0 : Z_DEFLATED;
    const int level = isDirectory ? 0 : Z_DEFAULT_COMPRESSION;
    const int windowBits = isDirectory ? 0 : -MAX_WBITS;
    const int memLevel = isDirectory ? 0 : DEF_MEM_LEVEL;
    const int strategy = isDirectory ? 0 : Z_DEFAULT_STRATEGY;
    const int needsZip64 = fileSize >= 0xffffffffULL ? 1 : 0;
    int rc = zipOpenNewFileInZip4_64(archive, name.c_str(), &fileInfo, nullptr, 0, nullptr, 0,
                                     nullptr, method, level, 0, windowBits, memLevel, strategy,
                                     nullptr, 0, 0, kZipUtf8Flag, needsZip64);
    if (rc != ZIP_OK) {
        errOut = "failed to open zip entry";
        return false;
    }
    return true;
}

bool closeZipEntry(zipFile archive, std::string& errOut) {
    if (zipCloseFileInZip(archive) != ZIP_OK) {
        errOut = "failed to close zip entry";
        return false;
    }
    return true;
}

bool writeSourceToArchive(ProviderManager& provMgr, zipFile archive, const std::string& fullPath,
                          const std::string& archivePath, uint64_t overallTotal,
                          uint64_t& overallDone, const ZipWriteProgressCb& progressCb,
                          std::string& errOut) {
    FileStatInfo info;
    if (!provMgr.statPath(fullPath, info, errOut))
        return false;
    if (info.isDirectory) {
        if (!openZipEntry(archive, archivePath, 0, true, errOut))
            return false;
        if (!closeZipEntry(archive, errOut))
            return false;

        auto entries = provMgr.listDir(fullPath, errOut);
        if (!errOut.empty())
            return false;
        for (const auto& entry : entries) {
            if (!isValidArchiveComponent(entry.name)) {
                errOut = "invalid archive entry name";
                return false;
            }
            const std::string childSrc = joinPath(fullPath, entry.name);
            const std::string childArchive = joinArchivePath(archivePath, entry.name);
            if (!writeSourceToArchive(provMgr, archive, childSrc, childArchive, overallTotal,
                                      overallDone, progressCb, errOut)) {
                return false;
            }
        }
        return true;
    }

    if (progressCb) {
        ZipWriteProgress progress;
        progress.currentPath = fullPath;
        progress.currentFileBytes = 0;
        progress.currentFileTotalBytes = info.size;
        progress.overallBytes = overallDone;
        progress.overallTotalBytes = overallTotal;
        if (!progressCb(progress)) {
            errOut = "interrupted";
            return false;
        }
    }

    if (!openZipEntry(archive, archivePath, info.size, false, errOut))
        return false;

    auto reader = provMgr.openSequentialRead(fullPath, 0, errOut);
    if (!reader) {
        std::string closeErr;
        zipCloseFileInZip(archive);
        return false;
    }

    std::vector<unsigned char> buffer(kZipWriteBufferSize);
    uint64_t fileDone = 0;
    uint64_t remaining = info.size;
    while (remaining > 0) {
        const std::size_t chunk = static_cast<std::size_t>(
            std::min<uint64_t>(remaining, static_cast<uint64_t>(buffer.size())));
        if (!reader->read(buffer.data(), chunk, errOut)) {
            std::string closeErr;
            zipCloseFileInZip(archive);
            return false;
        }
        if (zipWriteInFileInZip(archive, buffer.data(), static_cast<unsigned int>(chunk)) != ZIP_OK) {
            errOut = "failed to write zip entry";
            std::string closeErr;
            zipCloseFileInZip(archive);
            return false;
        }

        fileDone += chunk;
        overallDone += chunk;
        remaining -= chunk;
        if (progressCb) {
            ZipWriteProgress progress;
            progress.currentPath = fullPath;
            progress.currentFileBytes = fileDone;
            progress.currentFileTotalBytes = info.size;
            progress.overallBytes = overallDone;
            progress.overallTotalBytes = overallTotal;
            if (!progressCb(progress)) {
                errOut = "interrupted";
                std::string closeErr;
                zipCloseFileInZip(archive);
                return false;
            }
        }
    }

    if (!closeZipEntry(archive, errOut))
        return false;
    return true;
}

} // namespace

bool createZipArchive(ProviderManager& provMgr, const std::string& destFullPath,
                      const std::vector<ZipWriteSource>& sources, std::string& errOut,
                      const ZipWriteProgressCb& progressCb) {
    errOut.clear();
    if (sources.empty()) {
        errOut = "no sources selected";
        return false;
    }

    std::string destRelPath;
    FileProvider* destProvider = provMgr.resolveProvider(destFullPath, destRelPath);
    if (!destProvider) {
        errOut = "unknown destination provider";
        return false;
    }
    if (destProvider->isReadOnly()) {
        errOut = "destination provider is read-only";
        return false;
    }
    if (!destProvider->supportsPartialWrite()) {
        errOut = "destination provider does not support partial zip writes";
        return false;
    }

    uint64_t overallTotal = 0;
    for (const auto& source : sources) {
        if (source.fullPath.empty() || !isValidArchivePath(source.archiveRootName)) {
            errOut = "invalid archive entry name";
            return false;
        }
        if (!scanSourceBytes(provMgr, source.fullPath, overallTotal, errOut))
            return false;
    }

    ArchiveWriteContext context;
    context.provider = destProvider;
    context.relPath = destRelPath;

    zlib_filefunc64_def filefuncs {};
    filefuncs.zopen64_file = archiveWriteOpen64;
    filefuncs.zread_file = archiveWriteRead;
    filefuncs.zwrite_file = archiveWriteWrite;
    filefuncs.ztell64_file = archiveWriteTell64;
    filefuncs.zseek64_file = archiveWriteSeek64;
    filefuncs.zclose_file = archiveWriteClose;
    filefuncs.zerror_file = archiveWriteError;
    filefuncs.opaque = &context;

    zipFile archive = zipOpen2_64(destFullPath.c_str(), APPEND_STATUS_CREATE, nullptr, &filefuncs);
    if (!archive) {
        errOut = "failed to create zip archive";
        return false;
    }

    bool success = true;
    uint64_t overallDone = 0;
    for (const auto& source : sources) {
        if (!writeSourceToArchive(provMgr, archive, source.fullPath, source.archiveRootName,
                                  overallTotal, overallDone, progressCb, errOut)) {
            success = false;
            break;
        }
    }

    const int closeRc = zipClose(archive, nullptr);
    if (success && closeRc != ZIP_OK) {
        errOut = "failed to finalize zip archive";
        success = false;
    }
    return success;
}

} // namespace xxplore::fs
