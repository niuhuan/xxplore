#include "fs/zip_provider.hpp"
#include "fs/provider_manager.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <minizip/ioapi.h>
#include <minizip/unzip.h>

namespace xxplore {
namespace fs {

namespace {

constexpr std::size_t kAppletZipMaxNodes = 4096;
constexpr std::size_t kAppletZipMaxPathBytes = 256 * 1024;
constexpr std::size_t kNormalZipMaxNodes = 65536;
constexpr std::size_t kNormalZipMaxPathBytes = 8 * 1024 * 1024;
constexpr std::size_t kZipSkipBufferSize = 64 * 1024;

struct ArchiveSource {
    ProviderManager* provMgr = nullptr;
    std::string outerPath;
    uint64_t size = 0;
};

struct ArchiveReadStream {
    ArchiveSource* source = nullptr;
    uint64_t offset = 0;
    int error = 0;
};

class ScopedArchive {
public:
    ~ScopedArchive() {
        if (file)
            unzClose(file);
    }

    std::shared_ptr<ArchiveSource> source;
    unzFile file = nullptr;
};

struct ZipNode {
    std::string path;
    std::string name;
    bool isDirectory = false;
    uint64_t size = 0;
    bool hasSize = false;
    bool encrypted = false;
    bool hasFilePos = false;
    unz64_file_pos filePos {};
    std::vector<std::string> children;
};

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool isDrivePath(const std::string& value) {
    return value.size() >= 2 &&
           ((value[0] >= 'A' && value[0] <= 'Z') || (value[0] >= 'a' && value[0] <= 'z')) &&
           value[1] == ':';
}

bool normalizeZipEntryName(const std::string& rawName, std::string& outPath,
                           bool& isDirectory, std::string& errOut) {
    outPath.clear();
    errOut.clear();
    isDirectory = false;
    if (rawName.empty()) {
        errOut = "empty zip entry name";
        return false;
    }

    std::string value = rawName;
    std::replace(value.begin(), value.end(), '\\', '/');
    if (!value.empty() && value.back() == '/') {
        isDirectory = true;
        while (!value.empty() && value.back() == '/')
            value.pop_back();
    }

    if (value.empty()) {
        outPath = "/";
        isDirectory = true;
        return true;
    }
    if (value.front() == '/' || isDrivePath(value)) {
        errOut = "zip entry uses absolute path";
        return false;
    }

    std::vector<std::string> components;
    std::size_t start = 0;
    while (start <= value.size()) {
        std::size_t slash = value.find('/', start);
        std::string part = slash == std::string::npos
                               ? value.substr(start)
                               : value.substr(start, slash - start);
        if (part.empty() || part == "." || part == "..") {
            errOut = "zip entry contains unsafe path component";
            return false;
        }
        components.push_back(std::move(part));
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }

    outPath = "/";
    for (std::size_t i = 0; i < components.size(); ++i) {
        outPath += components[i];
        if (i + 1 < components.size())
            outPath += '/';
    }
    return true;
}

std::string normalizeProviderPath(const std::string& path) {
    if (path.empty() || path == "/")
        return "/";

    std::string value = path;
    std::replace(value.begin(), value.end(), '\\', '/');
    if (value.front() != '/')
        value.insert(value.begin(), '/');
    while (value.size() > 1 && value.back() == '/')
        value.pop_back();
    return value;
}

std::string parentNodePath(const std::string& path) {
    if (path.empty() || path == "/")
        return "/";
    std::size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0)
        return "/";
    return path.substr(0, slash);
}

std::string baseName(const std::string& path) {
    if (path.empty() || path == "/")
        return {};
    std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

void addUniqueChild(ZipNode& parent, const std::string& childPath) {
    if (std::find(parent.children.begin(), parent.children.end(), childPath) != parent.children.end())
        return;
    parent.children.push_back(childPath);
}

voidpf ZCALLBACK zipOpen64(voidpf opaque, const void* filename, int mode) {
    (void)filename;
    if (!opaque || (mode & ZLIB_FILEFUNC_MODE_READ) == 0)
        return nullptr;

    auto* stream = new ArchiveReadStream();
    stream->source = static_cast<ArchiveSource*>(opaque);
    stream->offset = 0;
    stream->error = 0;
    return stream;
}

uLong ZCALLBACK zipRead(voidpf opaque, voidpf streamPtr, void* buf, uLong size) {
    (void)opaque;
    auto* stream = static_cast<ArchiveReadStream*>(streamPtr);
    if (!stream || !stream->source || !stream->source->provMgr)
        return 0;

    if (stream->offset >= stream->source->size)
        return 0;

    uLong toRead = size;
    uint64_t remaining = stream->source->size - stream->offset;
    if (remaining < static_cast<uint64_t>(toRead))
        toRead = static_cast<uLong>(remaining);

    std::string err;
    if (!stream->source->provMgr->readFile(stream->source->outerPath, stream->offset, toRead, buf, err)) {
        stream->error = 1;
        return 0;
    }

    stream->offset += toRead;
    return toRead;
}

uLong ZCALLBACK zipWrite(voidpf opaque, voidpf streamPtr, const void* buf, uLong size) {
    (void)opaque;
    (void)streamPtr;
    (void)buf;
    (void)size;
    return 0;
}

ZPOS64_T ZCALLBACK zipTell64(voidpf opaque, voidpf streamPtr) {
    (void)opaque;
    auto* stream = static_cast<ArchiveReadStream*>(streamPtr);
    return stream ? static_cast<ZPOS64_T>(stream->offset) : 0;
}

long ZCALLBACK zipSeek64(voidpf opaque, voidpf streamPtr, ZPOS64_T offset, int origin) {
    (void)opaque;
    auto* stream = static_cast<ArchiveReadStream*>(streamPtr);
    if (!stream || !stream->source)
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
        next = stream->source->size + static_cast<uint64_t>(offset);
        break;
    default:
        return -1;
    }

    if (next > stream->source->size)
        return -1;
    stream->offset = next;
    return 0;
}

int ZCALLBACK zipClose(voidpf opaque, voidpf streamPtr) {
    (void)opaque;
    delete static_cast<ArchiveReadStream*>(streamPtr);
    return 0;
}

int ZCALLBACK zipError(voidpf opaque, voidpf streamPtr) {
    (void)opaque;
    auto* stream = static_cast<ArchiveReadStream*>(streamPtr);
    return stream ? stream->error : 0;
}

class ZipProvider final : public FileProvider {
public:
    ZipProvider(ProviderManager* provMgr, std::string outerPath, bool appletMode)
        : provMgr_(provMgr), outerPath_(std::move(outerPath)), appletMode_(appletMode) {}

    ProviderKind kind() const override { return ProviderKind::Zip; }
    std::string providerId() const override { return "zip|" + outerPath_; }
    std::string displayPrefix() const override { return outerPath_ + ":"; }
    bool isReadOnly() const override { return true; }
    bool allowsSelection() const override { return true; }

    ProviderCapabilities capabilities() const override {
        ProviderCapabilities caps;
        caps.canReadRange = true;
        caps.canReadSequential = true;
        caps.canWrite = false;
        caps.canPartialWrite = false;
        caps.canCreateDirectory = false;
        caps.canDelete = false;
        caps.canRename = false;
        caps.usesUtf8Paths = true;
        caps.canInstallFromSource = false;
        return caps;
    }

    std::vector<FileEntry> listDir(const std::string& path, std::string& errOut) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ensureIndexLocked(errOut))
            return {};

        const std::string normalized = normalizeProviderPath(path);
        auto it = nodes_.find(normalized);
        if (it == nodes_.end()) {
            errOut = "zip path not found";
            return {};
        }
        if (!it->second.isDirectory) {
            errOut = "zip path is not a directory";
            return {};
        }

        std::vector<FileEntry> result;
        result.reserve(it->second.children.size());
        for (const auto& childPath : it->second.children) {
            auto childIt = nodes_.find(childPath);
            if (childIt == nodes_.end())
                continue;
            FileEntry entry;
            entry.name = childIt->second.name;
            entry.isDirectory = childIt->second.isDirectory;
            entry.size = childIt->second.size;
            entry.hasSize = childIt->second.hasSize;
            result.push_back(std::move(entry));
        }

        auto cmpNoCase = [](const FileEntry& a, const FileEntry& b) {
            std::string la = toLower(a.name);
            std::string lb = toLower(b.name);
            if (la != lb)
                return la < lb;
            return a.name < b.name;
        };
        std::sort(result.begin(), result.end(), cmpNoCase);
        return result;
    }

    bool statPath(const std::string& path, FileStatInfo& out, std::string& errOut) override {
        std::lock_guard<std::mutex> lock(mutex_);
        out = {};
        if (!ensureIndexLocked(errOut))
            return false;

        const std::string normalized = normalizeProviderPath(path);
        auto it = nodes_.find(normalized);
        if (it == nodes_.end()) {
            errOut = "zip path not found";
            return false;
        }

        out.exists = true;
        out.isDirectory = it->second.isDirectory;
        out.size = it->second.size;
        return true;
    }

    bool createDirectory(const std::string& path, std::string& errOut) override {
        (void)path;
        errOut = "zip provider is read-only";
        return false;
    }

    bool removeAll(const std::string& path, std::string& errOut) override {
        (void)path;
        errOut = "zip provider is read-only";
        return false;
    }

    bool renamePath(const std::string& from, const std::string& to, std::string& errOut) override {
        (void)from;
        (void)to;
        errOut = "zip provider is read-only";
        return false;
    }

    bool readFile(const std::string& path, uint64_t offset, size_t size, void* outBuffer,
                  std::string& errOut) override {
        auto reader = openSequentialRead(path, offset, errOut);
        if (!reader)
            return false;
        return reader->read(outBuffer, size, errOut);
    }

    std::unique_ptr<SequentialFileReader>
    openSequentialRead(const std::string& path, uint64_t offset, std::string& errOut) override;

    bool writeFile(const std::string& path, const void* data, size_t size,
                   std::string& errOut) override {
        (void)path;
        (void)data;
        (void)size;
        errOut = "zip provider is read-only";
        return false;
    }

private:
    class ZipSequentialReader final : public SequentialFileReader {
    public:
        ZipSequentialReader(ZipProvider* provider, std::string path, uint64_t offset)
            : provider_(provider), path_(std::move(path)), offset_(offset) {}

        bool open(std::string& errOut) {
            if (!provider_) {
                errOut = "missing zip provider";
                return false;
            }
            return provider_->openEntryReader(path_, offset_, archive_, errOut);
        }

        bool read(void* outBuffer, size_t size, std::string& errOut) override {
            if (!archive_.file) {
                errOut = "zip file reader not open";
                return false;
            }

            auto* out = static_cast<unsigned char*>(outBuffer);
            size_t totalRead = 0;
            while (totalRead < size) {
                int rc = unzReadCurrentFile(archive_.file, out + totalRead,
                                            static_cast<unsigned>(size - totalRead));
                if (rc < 0) {
                    errOut = "zip entry read failed";
                    return false;
                }
                if (rc == 0) {
                    errOut = "unexpected zip eof";
                    return false;
                }
                totalRead += static_cast<size_t>(rc);
            }
            return true;
        }

    private:
        ZipProvider* provider_ = nullptr;
        std::string path_;
        uint64_t offset_ = 0;
        ScopedArchive archive_;
    };

    bool ensureIndexLocked(std::string& errOut) {
        if (indexed_) {
            errOut.clear();
            return true;
        }
        if (!buildIndexLocked(errOut))
            return false;
        indexed_ = true;
        return true;
    }

    bool buildIndexLocked(std::string& errOut) {
        nodes_.clear();
        nodes_["/"] = ZipNode{"/", "", true, 0, false, false, false, {}, {}};
        std::size_t nodeBudget = appletMode_ ? kAppletZipMaxNodes : kNormalZipMaxNodes;
        std::size_t pathBudget = appletMode_ ? kAppletZipMaxPathBytes : kNormalZipMaxPathBytes;
        std::size_t pathBytes = 0;
        auto isCancelled = [&]() {
            return provMgr_ && provMgr_->isPathLoadCancelled();
        };

        ScopedArchive archive;
        if (!openArchive(archive, errOut))
            return false;

        unz_global_info64 globalInfo {};
        if (unzGetGlobalInfo64(archive.file, &globalInfo) != UNZ_OK) {
            errOut = "failed to read zip global info";
            return false;
        }

        int rc = unzGoToFirstFile(archive.file);
        if (rc == UNZ_END_OF_LIST_OF_FILE)
            return true;
        if (rc != UNZ_OK) {
            errOut = "failed to seek first zip entry";
            return false;
        }

        for (;;) {
            if (isCancelled()) {
                errOut = "cancelled";
                return false;
            }

            unz_file_info64 info {};
            rc = unzGetCurrentFileInfo64(archive.file, &info, nullptr, 0, nullptr, 0, nullptr, 0);
            if (rc != UNZ_OK) {
                errOut = "failed to read zip entry info";
                return false;
            }

            std::vector<char> nameBuffer(static_cast<std::size_t>(info.size_filename) + 1, 0);
            rc = unzGetCurrentFileInfo64(archive.file, &info, nameBuffer.data(),
                                         static_cast<uLong>(nameBuffer.size()),
                                         nullptr, 0, nullptr, 0);
            if (rc != UNZ_OK) {
                errOut = "failed to read zip entry info";
                return false;
            }

            unz64_file_pos filePos {};
            if (unzGetFilePos64(archive.file, &filePos) != UNZ_OK) {
                errOut = "failed to read zip file position";
                return false;
            }

            std::string normalizedPath;
            bool isDirectory = false;
            if (!normalizeZipEntryName(nameBuffer.data(), normalizedPath, isDirectory, errOut))
                return false;
            if (normalizedPath == "/") {
                rc = unzGoToNextFile(archive.file);
                if (rc == UNZ_END_OF_LIST_OF_FILE)
                    break;
                if (rc != UNZ_OK) {
                    errOut = "failed to iterate zip entries";
                    return false;
                }
                continue;
            }

            std::vector<std::string> chain;
            std::string current = "/";
            std::size_t start = 1;
            while (start <= normalizedPath.size()) {
                std::size_t slash = normalizedPath.find('/', start);
                std::string part = slash == std::string::npos
                                       ? normalizedPath.substr(start)
                                       : normalizedPath.substr(start, slash - start);
                if (part.empty())
                    break;
                current = current == "/" ? "/" + part : current + "/" + part;
                chain.push_back(current);
                if (slash == std::string::npos)
                    break;
                start = slash + 1;
            }

            for (std::size_t i = 0; i < chain.size(); ++i) {
                if (isCancelled()) {
                    errOut = "cancelled";
                    return false;
                }
                const std::string& nodePath = chain[i];
                bool nodeIsDir = i + 1 < chain.size() || isDirectory;
                auto [it, inserted] = nodes_.emplace(nodePath, ZipNode{});
                if (inserted) {
                    if (nodes_.size() > nodeBudget) {
                        errOut = appletMode_ ? "zip is too large to browse in applet mode"
                                             : "zip tree is too large";
                        return false;
                    }
                    pathBytes += nodePath.size();
                    if (pathBytes > pathBudget) {
                        errOut = appletMode_ ? "zip is too large to browse in applet mode"
                                             : "zip tree is too large";
                        return false;
                    }
                    it->second.path = nodePath;
                    it->second.name = baseName(nodePath);
                }
                if (nodeIsDir) {
                    it->second.isDirectory = true;
                    it->second.hasSize = false;
                    it->second.size = 0;
                } else {
                    it->second.isDirectory = false;
                    it->second.size = static_cast<uint64_t>(info.uncompressed_size);
                    it->second.hasSize = true;
                    it->second.encrypted = (info.flag & 1U) != 0;
                    it->second.filePos = filePos;
                    it->second.hasFilePos = true;
                }

                const std::string parent = parentNodePath(nodePath);
                auto parentIt = nodes_.find(parent);
                if (parentIt != nodes_.end())
                    addUniqueChild(parentIt->second, nodePath);
            }

            rc = unzGoToNextFile(archive.file);
            if (rc == UNZ_END_OF_LIST_OF_FILE)
                break;
            if (rc != UNZ_OK) {
                errOut = "failed to iterate zip entries";
                return false;
            }
        }

        return true;
    }

    bool openArchive(ScopedArchive& archive, std::string& errOut) const {
        if (!provMgr_) {
            errOut = "missing provider manager";
            return false;
        }

        FileStatInfo info;
        if (!provMgr_->statPath(outerPath_, info, errOut))
            return false;
        if (!info.exists) {
            errOut = "zip archive does not exist";
            return false;
        }
        if (info.isDirectory) {
            errOut = "zip archive path is a directory";
            return false;
        }

        archive.source = std::make_shared<ArchiveSource>();
        archive.source->provMgr = provMgr_;
        archive.source->outerPath = outerPath_;
        archive.source->size = info.size;

        zlib_filefunc64_def funcs {};
        funcs.zopen64_file = zipOpen64;
        funcs.zread_file = zipRead;
        funcs.zwrite_file = zipWrite;
        funcs.ztell64_file = zipTell64;
        funcs.zseek64_file = zipSeek64;
        funcs.zclose_file = zipClose;
        funcs.zerror_file = zipError;
        funcs.opaque = archive.source.get();

        archive.file = unzOpen2_64(outerPath_.c_str(), &funcs);
        if (!archive.file) {
            errOut = "failed to open zip archive";
            return false;
        }
        return true;
    }

    bool openEntryReader(const std::string& path, uint64_t offset, ScopedArchive& archive,
                         std::string& errOut) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ensureIndexLocked(errOut))
            return false;

        const std::string normalized = normalizeProviderPath(path);
        auto it = nodes_.find(normalized);
        if (it == nodes_.end() || it->second.isDirectory || !it->second.hasFilePos) {
            errOut = "zip entry is not a file";
            return false;
        }
        if (it->second.encrypted) {
            errOut = "encrypted zip entries are unsupported";
            return false;
        }
        if (offset > it->second.size) {
            errOut = "zip read offset exceeds file size";
            return false;
        }

        if (!openArchive(archive, errOut))
            return false;
        if (unzGoToFilePos64(archive.file, &it->second.filePos) != UNZ_OK) {
            errOut = "failed to locate zip entry";
            return false;
        }
        if (unzOpenCurrentFile(archive.file) != UNZ_OK) {
            errOut = "failed to open zip entry stream";
            return false;
        }

        if (offset == 0)
            return true;

        std::vector<unsigned char> skipBuffer(kZipSkipBufferSize);
        uint64_t remaining = offset;
        while (remaining > 0) {
            unsigned chunk = static_cast<unsigned>(
                std::min<uint64_t>(remaining, static_cast<uint64_t>(skipBuffer.size())));
            int rc = unzReadCurrentFile(archive.file, skipBuffer.data(), chunk);
            if (rc <= 0) {
                errOut = "failed to skip zip entry bytes";
                return false;
            }
            remaining -= static_cast<uint64_t>(rc);
        }
        return true;
    }

    ProviderManager* provMgr_ = nullptr;
    std::string outerPath_;
    bool appletMode_ = false;
    bool indexed_ = false;
    std::unordered_map<std::string, ZipNode> nodes_;
    mutable std::mutex mutex_;
};

} // namespace

std::unique_ptr<SequentialFileReader>
ZipProvider::openSequentialRead(const std::string& path, uint64_t offset, std::string& errOut) {
    auto reader = std::make_unique<ZipSequentialReader>(this, path, offset);
    if (!reader->open(errOut))
        return nullptr;
    return reader;
}

std::shared_ptr<FileProvider> createZipProvider(ProviderManager* provMgr,
                                                const std::string& archiveFullPath,
                                                bool appletMode,
                                                std::string& errOut) {
    auto provider = std::make_shared<ZipProvider>(provMgr, archiveFullPath, appletMode);
    errOut.clear();
    return provider;
}

} // namespace fs
} // namespace xxplore
