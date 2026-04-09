#include "install/backend.hpp"
#include "ui/installer_screen.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdarg>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <zstd.h>

#include <switch.h>

namespace xxplore {

namespace {

void debugPrint(const char* tag, const char* fmt, ...) {
#ifdef XXPLORE_DEBUG
    std::printf("[%s] ", tag);
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
#else
    (void)tag;
    (void)fmt;
#endif
}

[[noreturn]] void throwFormatted(const char* func, int line, const std::string& msg) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s:%d: %s", func, line, msg.c_str());
#ifdef XXPLORE_DEBUG
    std::printf("[install-error] %s\n", buf);
#endif
    throw std::runtime_error(buf);
}

#define XP_THROW(msg) throwFormatted(__func__, __LINE__, (msg))
#define XP_ASSERT_OK(expr, msg) \
    do { \
        Result _rc = (expr); \
        if (R_FAILED(_rc)) { \
            char _buf[256]; \
            std::snprintf(_buf, sizeof(_buf), "%s (0x%08x)", (msg), _rc); \
            throwFormatted(__func__, __LINE__, _buf); \
        } \
    } while (0)

#ifdef XXPLORE_DEBUG
[[maybe_unused]] void printBytes(u8* bytes, size_t size, bool includeHeader) {
    if (includeHeader) {
        std::printf("\n\n00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F\n");
        std::printf("-----------------------------------------------\n");
    }
    for (size_t i = 0; i < size; i++) {
        std::printf("%02x ", bytes[i]);
        if (((i + 1) % 16) == 0)
            std::printf("\n");
    }
    std::printf("\n");
}
#else
[[maybe_unused]] void printBytes(u8*, size_t, bool) {}
#endif

#ifdef XXPLORE_DEBUG
#define LOG_DEBUG(format, ...) \
    do { \
        std::printf("%s:%d: ", __func__, __LINE__); \
        std::printf((format), ##__VA_ARGS__); \
    } while (0)
#else
#define LOG_DEBUG(format, ...) do { } while (0)
#endif

struct PFS0FileEntry {
    u64 dataOffset;
    u64 fileSize;
    u32 stringTableOffset;
    u32 padding;
} NX_PACKED;

struct PFS0BaseHeader {
    u32 magic;
    u32 numFiles;
    u32 stringTableSize;
    u32 reserved;
} NX_PACKED;

#define MAGIC_HFS0 0x30534648
#define NCA_HEADER_SIZE 0x4000
#define MAGIC_NCA3 0x3341434E

struct HFS0FileEntry {
    u64 dataOffset;
    u64 fileSize;
    u32 stringTableOffset;
    u32 hashedSize;
    u64 padding;
    unsigned char hash[0x20];
} NX_PACKED;

struct HFS0BaseHeader {
    u32 magic;
    u32 numFiles;
    u32 stringTableSize;
    u32 reserved;
} NX_PACKED;

NX_INLINE const HFS0FileEntry* hfs0GetFileEntry(const HFS0BaseHeader* header, u32 i) {
    if (i >= header->numFiles)
        return nullptr;
    return reinterpret_cast<const HFS0FileEntry*>(header + 0x1 + i * 0x4);
}

NX_INLINE const char* hfs0GetStringTable(const HFS0BaseHeader* header) {
    return reinterpret_cast<const char*>(header + 0x1 + header->numFiles * 0x4);
}

NX_INLINE const char* hfs0GetFileName(const HFS0BaseHeader* header, const HFS0FileEntry* entry) {
    return hfs0GetStringTable(header) + entry->stringTableOffset;
}

struct NcaFsHeader {
    u8 _0x0;
    u8 _0x1;
    u8 partition_type;
    u8 fs_type;
    u8 crypt_type;
    u8 _0x5[0x3];
    u8 superblock_data[0x138];
    union {
        u64 section_ctr;
        struct {
            u32 section_ctr_low;
            u32 section_ctr_high;
        };
    };
    u8 _0x148[0xB8];
} NX_PACKED;

struct NcaSectionEntry {
    u32 media_start_offset;
    u32 media_end_offset;
    u8 _0x8[0x8];
} NX_PACKED;

struct NcaHeader {
    u8 fixed_key_sig[0x100];
    u8 npdm_key_sig[0x100];
    u32 magic;
    u8 distribution;
    u8 content_type;
    u8 m_cryptoType;
    u8 m_kaekIndex;
    u64 nca_size;
    u64 m_titleId;
    u8 _0x218[0x4];
    union {
        uint32_t sdk_version;
        struct {
            u8 sdk_revision;
            u8 sdk_micro;
            u8 sdk_minor;
            u8 sdk_major;
        };
    };
    u8 m_cryptoType2;
    u8 _0x221[0xF];
    u64 m_rightsId[2];
    NcaSectionEntry section_entries[4];
    u8 section_hashes[4 * 0x20];
    u8 m_keys[4 * 0x10];
    u8 _0x340[0xC0];
    NcaFsHeader fs_headers[4];
} NX_PACKED;

namespace data {

class ByteBuffer {
public:
    explicit ByteBuffer(size_t reserveSize = 0) : buffer(reserveSize) {}

    size_t GetSize() const { return buffer.size(); }
    u8* GetData() { return buffer.data(); }
    void Resize(size_t size) { buffer.resize(size, 0); }

    template <typename T>
    T Read(u64 offset) const {
        if (offset + sizeof(T) <= buffer.size())
            return *reinterpret_cast<const T*>(buffer.data() + offset);
        T value {};
        return value;
    }

    template <typename T>
    void Write(T value, u64 offset) {
        size_t required = static_cast<size_t>(offset) + sizeof(T);
        if (required > buffer.size())
            buffer.resize(required, 0);
        std::memcpy(buffer.data() + offset, &value, sizeof(T));
    }

    template <typename T>
    void Append(T value) {
        Write<T>(value, GetSize());
    }

private:
    std::vector<u8> buffer;
};

} // namespace data

namespace ipc {

Service g_esSrv {};
Service g_nsAppManSrv {};

Result esInitialize() {
    return smGetService(&g_esSrv, "es");
}

void esExit() {
    serviceClose(&g_esSrv);
}

Result esImportTicket(const void* tikBuf, size_t tikSize, const void* certBuf, size_t certSize) {
    return serviceDispatch(&g_esSrv, 1,
        .buffer_attrs = {
            SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
            SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
        },
        .buffers = {
            { tikBuf, tikSize },
            { certBuf, certSize },
        },
    );
}

enum NsApplicationRecordType : u8 {
    NsApplicationRecordType_Installed = 0x3,
};

struct ContentStorageRecord {
    NcmContentMetaKey metaRecord;
    u64 storageId;
};

Result nsextInitialize() {
    Result rc = nsInitialize();
    if (R_SUCCEEDED(rc)) {
        if (hosversionBefore(3, 0, 0))
            g_nsAppManSrv = *nsGetServiceSession_ApplicationManagerInterface();
        else
            rc = nsGetApplicationManagerInterface(&g_nsAppManSrv);
    }
    return rc;
}

void nsextExit() {
    if (hosversionAtLeast(3, 0, 0))
        serviceClose(&g_nsAppManSrv);
    nsExit();
}

Result nsPushApplicationRecord(u64 applicationId, NsApplicationRecordType eventType,
                               ContentStorageRecord* records, u32 count) {
    struct {
        u8  last_modified_event;
        u64 application_id;
    } in = { eventType, applicationId };

    return serviceDispatchIn(&g_nsAppManSrv, 16, in,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { records, count * sizeof(*records) } });
}

} // namespace ipc

namespace util {

template <class T>
T swapEndian(T value) {
    T out {};
    u8* dst = reinterpret_cast<u8*>(&out);
    u8* src = reinterpret_cast<u8*>(&value);
    for (size_t i = 0; i < sizeof(T); i++)
        dst[i] = src[sizeof(T) - i - 1];
    return out;
}

std::string GetNcaIdString(const NcmContentId& ncaId) {
    char buf[FS_MAX_PATH] = {};
    u64 lower = swapEndian(*reinterpret_cast<const u64*>(ncaId.c));
    u64 upper = swapEndian(*reinterpret_cast<const u64*>(ncaId.c + 8));
    std::snprintf(buf, sizeof(buf), "%016lx%016lx", lower, upper);
    return buf;
}

NcmContentId GetNcaIdFromString(const std::string& ncaIdStr) {
    NcmContentId ncaId {};
    char lower[17] = {};
    char upper[17] = {};
    std::memcpy(lower, ncaIdStr.c_str(), 16);
    std::memcpy(upper, ncaIdStr.c_str() + 16, 16);
    *reinterpret_cast<u64*>(ncaId.c) = swapEndian(strtoull(lower, nullptr, 16));
    *reinterpret_cast<u64*>(ncaId.c + 8) = swapEndian(strtoull(upper, nullptr, 16));
    return ncaId;
}

u64 GetBaseTitleId(u64 titleId, NcmContentMetaType contentMetaType) {
    switch (contentMetaType) {
    case NcmContentMetaType_Patch: return titleId ^ 0x800;
    case NcmContentMetaType_AddOnContent: return (titleId ^ 0x1000) & ~0xFFF;
    default: return titleId;
    }
}

namespace crypto {

class Keys {
public:
    Keys() {
        u8 kek[0x10] = {};
        splCryptoGenerateAesKek(headerKekSource, 0, 0, kek);
        splCryptoGenerateAesKey(kek, headerKeySource, headerKey);
        splCryptoGenerateAesKey(kek, headerKeySource + 0x10, headerKey + 0x10);
    }

    u8 headerKekSource[0x10] = { 0x1F, 0x12, 0x91, 0x3A, 0x4A, 0xCB, 0xF0, 0x0D, 0x4C, 0xDE, 0x3A, 0xF6, 0xD5, 0x23, 0x88, 0x2A };
    u8 headerKeySource[0x20] = { 0x5A, 0x3E, 0xD8, 0x4F, 0xDE, 0xC0, 0xD8, 0x26, 0x31, 0xF7, 0xE2, 0x5D, 0x19, 0x7B, 0xF5, 0xD0, 0x1C, 0x9B, 0x7B, 0xFA, 0xF6, 0x28, 0x18, 0x3D, 0x71, 0xF6, 0x4D, 0x73, 0xF1, 0x50, 0xB9, 0xD2 };
    u8 headerKey[0x20] = {};
};

class AesCtr {
public:
    AesCtr() : highValue(0), lowValue(0) {}
    explicit AesCtr(u64 iv) : highValue(util::swapEndian(iv)), lowValue(0) {}
    u64& high() { return highValue; }
    u64& low() { return lowValue; }
private:
    u64 highValue;
    u64 lowValue;
};

class Aes128Ctr {
public:
    Aes128Ctr(const u8* key, const AesCtr& iv) : counter(iv) {
        aes128CtrContextCreate(&ctx, key, &iv);
        seek(0);
    }

    void seek(u64 offset) {
        counter.low() = util::swapEndian(offset >> 4);
        aes128CtrContextResetCtr(&ctx, &counter);
    }

    void encrypt(void* dst, const void* src, size_t len) {
        aes128CtrCrypt(&ctx, dst, src, len);
    }

private:
    AesCtr counter;
    Aes128CtrContext ctx {};
};

class AesXtr {
public:
    AesXtr(const u8* key, bool encryptor) {
        aes128XtsContextCreate(&ctx, key, key + 0x10, encryptor);
    }

    void encrypt(void* dst, const void* src, size_t len, size_t sector, size_t sectorSize) {
        for (size_t i = 0; i < len; i += sectorSize) {
            aes128XtsContextResetSector(&ctx, sector++, true);
            aes128XtsEncrypt(&ctx, dst, src, sectorSize);
            dst = static_cast<u8*>(dst) + sectorSize;
            src = static_cast<const u8*>(src) + sectorSize;
        }
    }

    void decrypt(void* dst, const void* src, size_t len, size_t sector, size_t sectorSize) {
        for (size_t i = 0; i < len; i += sectorSize) {
            aes128XtsContextResetSector(&ctx, sector++, true);
            aes128XtsDecrypt(&ctx, dst, src, sectorSize);
            dst = static_cast<u8*>(dst) + sectorSize;
            src = static_cast<const u8*>(src) + sectorSize;
        }
    }

private:
    Aes128XtsContext ctx {};
};

} // namespace crypto

} // namespace util

namespace nxncm {

class ContentStorage {
public:
    explicit ContentStorage(NcmStorageId storageId) {
        XP_ASSERT_OK(ncmOpenContentStorage(&contentStorage, storageId), "Failed to open NCM ContentStorage");
    }

    ~ContentStorage() {
        serviceClose(&contentStorage.s);
    }

    NcmPlaceHolderId GeneratePlaceholderId() {
        NcmPlaceHolderId placeholderId {};
        XP_ASSERT_OK(ncmContentStorageGeneratePlaceHolderId(&contentStorage, &placeholderId),
                     "Failed to generate placeholder id");
        return placeholderId;
    }

    void CreatePlaceholder(const NcmContentId& placeholderId, const NcmPlaceHolderId& registeredId, size_t size) {
        XP_ASSERT_OK(ncmContentStorageCreatePlaceHolder(&contentStorage, &placeholderId, &registeredId, size),
                     "Failed to create placeholder");
    }

    void DeletePlaceholder(const NcmPlaceHolderId& placeholderId) {
        XP_ASSERT_OK(ncmContentStorageDeletePlaceHolder(&contentStorage, &placeholderId),
                     "Failed to delete placeholder");
    }

    void WritePlaceholder(const NcmPlaceHolderId& placeholderId, u64 offset, void* buffer, size_t bufferSize) {
        XP_ASSERT_OK(ncmContentStorageWritePlaceHolder(&contentStorage, &placeholderId, offset, buffer, bufferSize),
                     "Failed to write placeholder");
    }

    void Register(const NcmPlaceHolderId& placeholderId, const NcmContentId& registeredId) {
        XP_ASSERT_OK(ncmContentStorageRegister(&contentStorage, &registeredId, &placeholderId),
                     "Failed to register placeholder");
    }

    void Delete(const NcmContentId& registeredId) {
        XP_ASSERT_OK(ncmContentStorageDelete(&contentStorage, &registeredId),
                     "Failed to delete registered NCA");
    }

    bool Has(const NcmContentId& registeredId) {
        bool hasNca = false;
        XP_ASSERT_OK(ncmContentStorageHas(&contentStorage, &hasNca, &registeredId),
                     "Failed to check NCA presence");
        return hasNca;
    }

    bool HasPlaceholder(const NcmPlaceHolderId& placeholderId) {
        bool hasPlaceholder = false;
        XP_ASSERT_OK(ncmContentStorageHasPlaceHolder(&contentStorage, &hasPlaceholder, &placeholderId),
                     "Failed to check placeholder presence");
        return hasPlaceholder;
    }

    std::string GetPath(const NcmContentId& registeredId) {
        char pathBuf[FS_MAX_PATH] = {};
        XP_ASSERT_OK(ncmContentStorageGetPath(&contentStorage, pathBuf, FS_MAX_PATH, &registeredId),
                     "Failed to get NCA path");
        return pathBuf;
    }

private:
    NcmContentStorage contentStorage {};
};

struct PackagedContentInfo {
    u8 hash[0x20];
    NcmContentInfo content_info;
} NX_PACKED;

struct PackagedContentMetaHeader {
    u64 title_id;
    u32 version;
    u8  type;
    u8  _0xd;
    u16 extended_header_size;
    u16 content_count;
    u16 content_meta_count;
    u8  attributes;
    u8  storage_id;
    u8  install_type;
    bool comitted;
    u32 required_system_version;
    u32 _0x1c;
};

class ContentMeta {
public:
    ContentMeta() : bytes(sizeof(PackagedContentMetaHeader)) {}
    ContentMeta(u8* dataPtr, size_t size) : bytes(size) {
        if (size < sizeof(PackagedContentMetaHeader))
            XP_THROW("Content meta data size is too small");
        bytes.Resize(size);
        std::memcpy(bytes.GetData(), dataPtr, size);
    }

    PackagedContentMetaHeader GetPackagedContentMetaHeader() {
        return bytes.Read<PackagedContentMetaHeader>(0);
    }

    NcmContentMetaKey GetContentMetaKey() {
        NcmContentMetaKey metaRecord {};
        auto header = GetPackagedContentMetaHeader();
        metaRecord.id = header.title_id;
        metaRecord.version = header.version;
        metaRecord.type = static_cast<NcmContentMetaType>(header.type);
        return metaRecord;
    }

    std::vector<NcmContentInfo> GetContentInfos() {
        auto header = GetPackagedContentMetaHeader();
        std::vector<NcmContentInfo> infos;
        auto* packagedInfos = reinterpret_cast<PackagedContentInfo*>(
            bytes.GetData() + sizeof(PackagedContentMetaHeader) + header.extended_header_size);
        for (unsigned int i = 0; i < header.content_count; i++) {
            PackagedContentInfo packagedInfo = packagedInfos[i];
            if (static_cast<u8>(packagedInfo.content_info.content_type) <= 5)
                infos.push_back(packagedInfo.content_info);
        }
        return infos;
    }

    void GetInstallContentMeta(data::ByteBuffer& installContentMetaBuffer,
                               NcmContentInfo& cnmtContentInfo, bool ignoreReqFirmVersion) {
        auto packagedHeader = GetPackagedContentMetaHeader();
        auto contentInfos = GetContentInfos();

        NcmContentMetaHeader contentMetaHeader {};
        contentMetaHeader.extended_header_size = packagedHeader.extended_header_size;
        contentMetaHeader.content_count = static_cast<u16>(contentInfos.size() + 1);
        contentMetaHeader.content_meta_count = packagedHeader.content_meta_count;
        contentMetaHeader.attributes = packagedHeader.attributes;
        contentMetaHeader.storage_id = 0;

        installContentMetaBuffer.Append<NcmContentMetaHeader>(contentMetaHeader);
        installContentMetaBuffer.Resize(installContentMetaBuffer.GetSize() + contentMetaHeader.extended_header_size);

        auto* extendedHeaderSrc = bytes.GetData() + sizeof(PackagedContentMetaHeader);
        std::memcpy(installContentMetaBuffer.GetData() + sizeof(NcmContentMetaHeader),
                    extendedHeaderSrc, contentMetaHeader.extended_header_size);

        if (ignoreReqFirmVersion &&
            (packagedHeader.type == NcmContentMetaType_Application ||
             packagedHeader.type == NcmContentMetaType_Patch)) {
            installContentMetaBuffer.Write<u32>(0, sizeof(NcmContentMetaHeader) + 8);
        }

        installContentMetaBuffer.Append<NcmContentInfo>(cnmtContentInfo);
        for (const auto& info : contentInfos)
            installContentMetaBuffer.Append<NcmContentInfo>(info);

        if (packagedHeader.type == NcmContentMetaType_Patch) {
            auto* patchHeader = reinterpret_cast<NcmPatchMetaExtendedHeader*>(extendedHeaderSrc);
            installContentMetaBuffer.Resize(installContentMetaBuffer.GetSize() + patchHeader->extended_data_size);
        }
    }

private:
    data::ByteBuffer bytes;
};

} // namespace nxncm

namespace nxfs {

class IFile {
public:
    explicit IFile(FsFile& file) : fileHandle(file) {}
    ~IFile() { fsFileClose(&fileHandle); }

    void Read(u64 offset, void* buf, size_t size) {
        u64 sizeRead = 0;
        XP_ASSERT_OK(fsFileRead(&fileHandle, offset, buf, size, FsReadOption_None, &sizeRead),
                     "Failed to read file");
        if (sizeRead != size)
            XP_THROW("Read size mismatch");
    }

    s64 GetSize() {
        s64 sizeOut = 0;
        XP_ASSERT_OK(fsFileGetSize(&fileHandle, &sizeOut), "Failed to get file size");
        return sizeOut;
    }

private:
    FsFile fileHandle {};
};

class IDirectory {
public:
    explicit IDirectory(FsDir& dir) : dirHandle(dir) {}
    ~IDirectory() { fsDirClose(&dirHandle); }

    void Read(s64 inval, FsDirectoryEntry* buf, size_t numEntries) {
        XP_ASSERT_OK(fsDirRead(&dirHandle, &inval, numEntries, buf), "Failed to read directory");
    }

    u64 GetEntryCount() {
        s64 entryCount = 0;
        XP_ASSERT_OK(fsDirGetEntryCount(&dirHandle, &entryCount), "Failed to get directory entry count");
        return static_cast<u64>(entryCount);
    }

private:
    FsDir dirHandle {};
};

class IFileSystem {
public:
    IFileSystem() = default;
    ~IFileSystem() { fsFsClose(&fileSystem); }

    void OpenFileSystemWithId(std::string path, FsFileSystemType fileSystemType, u64 titleId) {
        if (path.length() >= FS_MAX_PATH)
            XP_THROW("Directory path is too long");
        path.reserve(FS_MAX_PATH);
        Result rc = 0;
        for (int attempt = 0; attempt < 6; ++attempt) {
            rc = fsOpenFileSystemWithId(&fileSystem, titleId, fileSystemType, path.c_str(),
                                        FsContentAttributes_All);
            if (R_SUCCEEDED(rc))
                return;
            svcSleepThread(50'000'000ULL);
        }
        char buf[512];
        std::snprintf(buf, sizeof(buf), "Failed to open file system with id: %s (0x%08x)",
                      path.c_str(), rc);
        throwFormatted(__func__, __LINE__, buf);
    }

    IFile OpenFile(std::string path) {
        if (path.length() >= FS_MAX_PATH)
            XP_THROW("File path is too long");
        path.reserve(FS_MAX_PATH);
        FsFile file {};
        XP_ASSERT_OK(fsFsOpenFile(&fileSystem, path.c_str(), FsOpenMode_Read, &file),
                     "Failed to open file");
        return IFile(file);
    }

    IDirectory OpenDirectory(std::string path, int flags) {
        if (path.length() >= FS_MAX_PATH)
            XP_THROW("Directory path is too long");
        path.reserve(FS_MAX_PATH);
        FsDir dir {};
        XP_ASSERT_OK(fsFsOpenDirectory(&fileSystem, path.c_str(), flags, &dir),
                     "Failed to open directory");
        return IDirectory(dir);
    }

private:
    FsFileSystem fileSystem {};
};

} // namespace nxfs

namespace install {
namespace nsp {

class SimpleFileSystem {
public:
    SimpleFileSystem(nxfs::IFileSystem& fs, std::string root, std::string absoluteRoot)
        : fileSystem(&fs), rootPath(std::move(root)), absoluteRootPath(std::move(absoluteRoot)) {}

    nxfs::IFile OpenFile(const std::string& path) {
        return fileSystem->OpenFile(rootPath + path);
    }

    std::string GetFileNameFromExtension(const std::string& path, const std::string& extension) {
        auto dir = fileSystem->OpenDirectory(rootPath + path, FsDirOpenMode_ReadFiles | FsDirOpenMode_ReadDirs);
        u64 entryCount = dir.GetEntryCount();
        auto entries = std::make_unique<FsDirectoryEntry[]>(entryCount);
        dir.Read(0, entries.get(), entryCount);

        for (u64 i = 0; i < entryCount; ++i) {
            const FsDirectoryEntry& entry = entries[i];
            std::string name = entry.name;
            if (entry.type == FsDirEntryType_Dir) {
                std::string subdirPath = path + name + "/";
                std::string found = GetFileNameFromExtension(subdirPath, extension);
                if (!found.empty())
                    return found;
                continue;
            }

            auto pos = name.find('.');
            if (entry.type == FsDirEntryType_File && pos != std::string::npos &&
                name.substr(pos + 1) == extension) {
                return name;
            }
        }
        return "";
    }

private:
    nxfs::IFileSystem* fileSystem;
    std::string rootPath;
    std::string absoluteRootPath;
};

nxncm::ContentMeta GetContentMetaFromNCA(const std::string& ncaPath) {
    nxfs::IFileSystem fileSystem;
    fileSystem.OpenFileSystemWithId(ncaPath, FsFileSystemType_ContentMeta, 0);
    SimpleFileSystem simpleFs(fileSystem, "/", ncaPath + "/");

    auto cnmtName = simpleFs.GetFileNameFromExtension("", "cnmt");
    if (cnmtName.empty())
        XP_THROW("Failed to find cnmt file inside content meta filesystem");
    auto cnmtFile = simpleFs.OpenFile(cnmtName);
    u64 cnmtSize = static_cast<u64>(cnmtFile.GetSize());

    data::ByteBuffer cnmtBuf;
    cnmtBuf.Resize(cnmtSize);
    cnmtFile.Read(0, cnmtBuf.GetData(), cnmtSize);
    return nxncm::ContentMeta(cnmtBuf.GetData(), cnmtBuf.GetSize());
}

class NSP {
public:
    virtual ~NSP() = default;
    virtual void StreamToPlaceholder(std::shared_ptr<nxncm::ContentStorage>& contentStorage,
                                     const NcmPlaceHolderId& placeholderId,
                                     const NcmContentId& contentId) = 0;
    virtual void BufferData(void* buf, off_t offset, size_t size) = 0;

    void RetrieveHeader() {
        headerBytes.resize(sizeof(PFS0BaseHeader), 0);
        BufferData(headerBytes.data(), 0, sizeof(PFS0BaseHeader));
        size_t remainingHeaderSize =
            GetBaseHeader()->numFiles * sizeof(PFS0FileEntry) + GetBaseHeader()->stringTableSize;
        headerBytes.resize(sizeof(PFS0BaseHeader) + remainingHeaderSize, 0);
        BufferData(headerBytes.data() + sizeof(PFS0BaseHeader), sizeof(PFS0BaseHeader), remainingHeaderSize);
    }

    const PFS0BaseHeader* GetBaseHeader() const {
        if (headerBytes.empty())
            XP_THROW("NSP header not loaded");
        return reinterpret_cast<const PFS0BaseHeader*>(headerBytes.data());
    }

    const PFS0FileEntry* GetFileEntry(unsigned int index) const {
        if (index >= GetBaseHeader()->numFiles)
            XP_THROW("NSP file entry index out of bounds");
        size_t fileEntryOffset = sizeof(PFS0BaseHeader) + index * sizeof(PFS0FileEntry);
        return reinterpret_cast<const PFS0FileEntry*>(headerBytes.data() + fileEntryOffset);
    }

    std::vector<const PFS0FileEntry*> GetFileEntriesByExtension(const std::string& extension) const {
        std::vector<const PFS0FileEntry*> entries;
        for (unsigned int i = 0; i < GetBaseHeader()->numFiles; i++) {
            const PFS0FileEntry* fileEntry = GetFileEntry(i);
            std::string name = GetFileEntryName(fileEntry);
            auto foundExtension = name.substr(name.find(".") + 1);
            if (foundExtension == extension)
                entries.push_back(fileEntry);
        }
        return entries;
    }

    const PFS0FileEntry* GetFileEntryByName(const std::string& name) const {
        for (unsigned int i = 0; i < GetBaseHeader()->numFiles; i++) {
            const PFS0FileEntry* fileEntry = GetFileEntry(i);
            if (GetFileEntryName(fileEntry) == name)
                return fileEntry;
        }
        return nullptr;
    }

    const PFS0FileEntry* GetFileEntryByNcaId(const NcmContentId& ncaId) const {
        std::string ncaIdStr = util::GetNcaIdString(ncaId);
        const char* names[] = { ".nca", ".cnmt.nca", ".ncz", ".cnmt.ncz" };
        for (const char* suffix : names) {
            if (const auto* entry = GetFileEntryByName(ncaIdStr + suffix))
                return entry;
        }
        return nullptr;
    }

    std::string GetFileEntryName(const PFS0FileEntry* fileEntry) const {
        u64 stringTableStart = sizeof(PFS0BaseHeader) + GetBaseHeader()->numFiles * sizeof(PFS0FileEntry);
        return reinterpret_cast<const char*>(headerBytes.data() + stringTableStart + fileEntry->stringTableOffset);
    }

    u64 GetDataOffset() const { return headerBytes.size(); }

private:
    std::vector<u8> headerBytes;
};

} // namespace nsp

namespace xci {

class XCI {
public:
    virtual ~XCI() = default;
    virtual void StreamToPlaceholder(std::shared_ptr<nxncm::ContentStorage>& contentStorage,
                                     const NcmPlaceHolderId& placeholderId,
                                     const NcmContentId& contentId) = 0;
    virtual void BufferData(void* buf, off_t offset, size_t size) = 0;

    void RetrieveHeader() {
        constexpr u64 hfs0Offset = 0xf000;
        std::vector<u8> headerBytes(sizeof(HFS0BaseHeader), 0);
        BufferData(headerBytes.data(), hfs0Offset, sizeof(HFS0BaseHeader));
        auto* header = reinterpret_cast<HFS0BaseHeader*>(headerBytes.data());
        if (header->magic != MAGIC_HFS0)
            XP_THROW("Invalid HFS0 header");

        size_t remaining = header->numFiles * sizeof(HFS0FileEntry) + header->stringTableSize;
        headerBytes.resize(sizeof(HFS0BaseHeader) + remaining, 0);
        BufferData(headerBytes.data() + sizeof(HFS0BaseHeader), hfs0Offset + sizeof(HFS0BaseHeader), remaining);
        header = reinterpret_cast<HFS0BaseHeader*>(headerBytes.data());
        for (unsigned int i = 0; i < header->numFiles; i++) {
            const HFS0FileEntry* entry = hfs0GetFileEntry(header, i);
            if (std::string(hfs0GetFileName(header, entry)) != "secure")
                continue;
            secureHeaderOffset = hfs0Offset + remaining + 0x10 + entry->dataOffset;
            secureHeaderBytes.resize(sizeof(HFS0BaseHeader), 0);
            BufferData(secureHeaderBytes.data(), secureHeaderOffset, sizeof(HFS0BaseHeader));
            if (GetSecureHeader()->magic != MAGIC_HFS0)
                XP_THROW("Invalid secure HFS0 header");
            remaining = GetSecureHeader()->numFiles * sizeof(HFS0FileEntry) + GetSecureHeader()->stringTableSize;
            secureHeaderBytes.resize(sizeof(HFS0BaseHeader) + remaining, 0);
            BufferData(secureHeaderBytes.data() + sizeof(HFS0BaseHeader),
                       secureHeaderOffset + sizeof(HFS0BaseHeader), remaining);
            return;
        }
        XP_THROW("Secure partition not found");
    }

    const HFS0BaseHeader* GetSecureHeader() const {
        if (secureHeaderBytes.empty())
            XP_THROW("XCI secure header not loaded");
        return reinterpret_cast<const HFS0BaseHeader*>(secureHeaderBytes.data());
    }

    const HFS0FileEntry* GetFileEntry(unsigned int index) const {
        return hfs0GetFileEntry(GetSecureHeader(), index);
    }

    std::vector<const HFS0FileEntry*> GetFileEntriesByExtension(const std::string& extension) const {
        std::vector<const HFS0FileEntry*> entries;
        for (unsigned int i = 0; i < GetSecureHeader()->numFiles; i++) {
            const HFS0FileEntry* entry = GetFileEntry(i);
            std::string name = GetFileEntryName(entry);
            auto foundExtension = name.substr(name.find(".") + 1);
            if (foundExtension == extension)
                entries.push_back(entry);
        }
        return entries;
    }

    const HFS0FileEntry* GetFileEntryByName(const std::string& name) const {
        for (unsigned int i = 0; i < GetSecureHeader()->numFiles; i++) {
            const HFS0FileEntry* entry = GetFileEntry(i);
            if (GetFileEntryName(entry) == name)
                return entry;
        }
        return nullptr;
    }

    const HFS0FileEntry* GetFileEntryByNcaId(const NcmContentId& ncaId) const {
        std::string ncaIdStr = util::GetNcaIdString(ncaId);
        const char* names[] = { ".nca", ".cnmt.nca", ".ncz", ".cnmt.ncz" };
        for (const char* suffix : names) {
            if (const auto* entry = GetFileEntryByName(ncaIdStr + suffix))
                return entry;
        }
        return nullptr;
    }

    std::string GetFileEntryName(const HFS0FileEntry* entry) const {
        return hfs0GetFileName(GetSecureHeader(), entry);
    }

    u64 GetDataOffset() const { return secureHeaderOffset + secureHeaderBytes.size(); }

private:
    u64 secureHeaderOffset = 0;
    std::vector<u8> secureHeaderBytes;
};

} // namespace xci
} // namespace install

class NcaBodyWriter {
public:
    NcaBodyWriter(const NcmContentId& ncaId, const NcmPlaceHolderId& placeholderId, u64 offset,
                  std::shared_ptr<nxncm::ContentStorage>& storage)
        : contentStorage(storage), contentId(ncaId), placeholderId(placeholderId), currentOffset(offset) {}
    virtual ~NcaBodyWriter() = default;

    virtual u64 write(const u8* ptr, u64 size) {
        if (contentStorage) {
            contentStorage->WritePlaceholder(placeholderId, currentOffset, const_cast<u8*>(ptr), size);
            currentOffset += size;
            return size;
        }
        return 0;
    }

protected:
    std::shared_ptr<nxncm::ContentStorage> contentStorage;
    NcmContentId contentId {};
    NcmPlaceHolderId placeholderId {};
    u64 currentOffset = 0;
};

class NczBodyWriter final : public NcaBodyWriter {
public:
    static constexpr u64 kHeaderMagic = 0x4E544345535A434EULL;

    NczBodyWriter(const NcmContentId& ncaId, const NcmPlaceHolderId& placeholderId, u64 offset,
                  std::shared_ptr<nxncm::ContentStorage>& storage)
        : NcaBodyWriter(ncaId, placeholderId, offset, storage) {
        dctx = ZSTD_createDCtx();
    }

    ~NczBodyWriter() override {
        close();
        for (auto* section : sections)
            delete section;
        ZSTD_freeDCtx(dctx);
    }

    u64 write(const u8* ptr, u64 size) override {
        while (!sectionsInitialized) {
            if (buffer.empty() && size >= sizeof(u64) * 2) {
                append(buffer, ptr, sizeof(u64) * 2);
                ptr += sizeof(u64) * 2;
                size -= sizeof(u64) * 2;
            }

            if (buffer.size() < sizeof(u64) * 2)
                return 0;

            auto* header = reinterpret_cast<NczHeader*>(buffer.data());
            size_t headerSize = static_cast<size_t>(header->size());
            size_t remainder = headerSize > buffer.size() ? headerSize - buffer.size() : 0;
            size_t copySize = std::min<u64>(remainder, size);
            append(buffer, ptr, copySize);
            ptr += copySize;
            size -= copySize;

            if (buffer.size() == headerSize) {
                for (u64 i = 0; i < header->sectionCount(); i++)
                    sections.push_back(new SectionContext(header->section(i)));
                sectionsInitialized = true;
                buffer.clear();
            } else {
                return 0;
            }
        }

        while (size > 0) {
            u64 chunk = std::min<u64>(size, 0x1000000 - buffer.size());
            append(buffer, ptr, chunk);
            ptr += chunk;
            size -= chunk;
            if (buffer.size() >= 0x1000000) {
                processChunk(buffer.data(), buffer.size());
                buffer.clear();
            }
        }
        return 0;
    }

    bool close() {
        if (!buffer.empty())
            processChunk(buffer.data(), buffer.size());
        encrypt(deflateBuffer.data(), deflateBuffer.size(), currentOffset);
        flush();
        return true;
    }

private:
    class NczHeader {
    public:
        static constexpr u64 MAGIC = kHeaderMagic;

        class Section {
        public:
            u64 offset;
            u64 size;
            u8  cryptoType;
            u8  padding1[7];
            u64 padding2;
            u8  cryptoKey[0x10];
            u8  cryptoCounter[0x10];
        } NX_PACKED;

        const bool isValid() const {
            return magic == MAGIC && sectionCountValue < 0xFFFF;
        }

        const u64 size() const {
            return sizeof(magic) + sizeof(sectionCountValue) + sizeof(Section) * sectionCountValue;
        }

        const Section& section(u64 i) const {
            return sectionsValue[i];
        }

        const u64 sectionCount() const {
            return sectionCountValue;
        }

    private:
        u64 magic;
        u64 sectionCountValue;
        Section sectionsValue[1];
    } NX_PACKED;

    class SectionContext : public NczHeader::Section {
    public:
        explicit SectionContext(const NczHeader::Section& section)
            : NczHeader::Section(section),
              crypto(section.cryptoKey, util::crypto::AesCtr(util::swapEndian(reinterpret_cast<const u64*>(section.cryptoCounter)[0]))) {}

        void encrypt(void* ptr, u64 size, u64 offset) {
            if (cryptoType != 3)
                return;
            crypto.seek(offset);
            crypto.encrypt(ptr, ptr, size);
        }

        util::crypto::Aes128Ctr crypto;
    };

    static void append(std::vector<u8>& out, const void* src, size_t size) {
        size_t oldSize = out.size();
        out.resize(oldSize + size);
        std::memcpy(out.data() + oldSize, src, size);
    }

    SectionContext& section(u64 offset) {
        for (auto* item : sections) {
            if (offset >= item->offset && offset < item->offset + item->size)
                return *item;
        }
        return *sections.front();
    }

    void encrypt(void* ptr, u64 size, u64 offset) {
        u8* start = static_cast<u8*>(ptr);
        while (size > 0) {
            auto& sec = section(offset);
            u64 sectionEnd = sec.offset + sec.size;
            u64 chunk = offset + size > sectionEnd ? sectionEnd - offset : size;
            sec.encrypt(start, chunk, offset);
            offset += chunk;
            start += chunk;
            size -= chunk;
        }
    }

    void flush() {
        if (!deflateBuffer.empty()) {
            contentStorage->WritePlaceholder(placeholderId, currentOffset, deflateBuffer.data(),
                                             deflateBuffer.size());
            currentOffset += deflateBuffer.size();
            deflateBuffer.clear();
        }
    }

    void processChunk(const u8* ptr, u64 size) {
        while (size > 0) {
            size_t readChunk = std::min<u64>(size, ZSTD_DStreamInSize());
            ZSTD_inBuffer input = { ptr, readChunk, 0 };
            while (input.pos < input.size) {
                std::vector<u8> out(ZSTD_DStreamOutSize());
                ZSTD_outBuffer output = { out.data(), out.size(), 0 };
                size_t ret = ZSTD_decompressStream(dctx, &output, &input);
                if (ZSTD_isError(ret))
                    XP_THROW(ZSTD_getErrorName(ret));
                size_t len = output.pos;
                u8* outPtr = out.data();
                while (len > 0) {
                    size_t writeChunk = std::min<size_t>(0x1000000 - deflateBuffer.size(), len);
                    append(deflateBuffer, outPtr, writeChunk);
                    if (deflateBuffer.size() >= 0x1000000) {
                        encrypt(deflateBuffer.data(), deflateBuffer.size(), currentOffset);
                        flush();
                    }
                    outPtr += writeChunk;
                    len -= writeChunk;
                }
            }
            size -= readChunk;
            ptr += readChunk;
        }
    }

    ZSTD_DCtx* dctx = nullptr;
    std::vector<u8> buffer;
    std::vector<u8> deflateBuffer;
    bool sectionsInitialized = false;
    std::vector<SectionContext*> sections;
};

class NcaWriter {
public:
    NcaWriter(const NcmContentId& ncaId, const NcmPlaceHolderId& placeholderId,
              std::shared_ptr<nxncm::ContentStorage>& storage)
        : contentId(ncaId), placeholderId(placeholderId), contentStorage(storage) {}
    ~NcaWriter() { close(); }

    u64 write(const u8* ptr, u64 size) {
        if (headerBuffer.size() < NCA_HEADER_SIZE) {
            u64 remainder = NCA_HEADER_SIZE - headerBuffer.size();
            u64 copySize = std::min<u64>(remainder, size);
            append(headerBuffer, ptr, copySize);
            ptr += copySize;
            size -= copySize;
            if (headerBuffer.size() == NCA_HEADER_SIZE)
                flushHeader();
        }

        if (size == 0)
            return 0;

        if (!writer) {
            if (size < sizeof(u64))
                XP_THROW("Not enough data to determine NCA body type");
            if (*reinterpret_cast<const u64*>(ptr) == NczBodyWriter::kHeaderMagic)
                writer = std::make_shared<NczBodyWriter>(contentId, placeholderId, NCA_HEADER_SIZE,
                                                         contentStorage);
            else
                writer = std::make_shared<NcaBodyWriter>(contentId, placeholderId, NCA_HEADER_SIZE,
                                                         contentStorage);
        }

        writer->write(ptr, size);
        return size;
    }

    void close() {
        if (closed)
            return;
        closed = true;

        if (writer) {
            writer.reset();
        } else if (!headerBuffer.empty() && !headerFlushed) {
            flushHeader();
        }
        headerBuffer.clear();
        contentStorage.reset();
    }

private:
    static void append(std::vector<u8>& out, const void* src, size_t size) {
        size_t oldSize = out.size();
        out.resize(oldSize + size);
        std::memcpy(out.data() + oldSize, src, size);
    }

    void flushHeader() {
        if (headerFlushed)
            return;
        NcaHeader header {};
        std::memcpy(&header, headerBuffer.data(), sizeof(header));
        util::crypto::AesXtr decryptor(util::crypto::Keys().headerKey, false);
        util::crypto::AesXtr encryptor(util::crypto::Keys().headerKey, true);
        decryptor.decrypt(&header, &header, sizeof(header), 0, 0x200);
        if (header.magic != MAGIC_NCA3)
            XP_THROW("Invalid NCA magic");
        debugPrint("nca", "create placeholder content=%s size=%llu", util::GetNcaIdString(contentId).c_str(),
                   static_cast<unsigned long long>(header.nca_size));
        contentStorage->CreatePlaceholder(contentId, placeholderId, header.nca_size);
        if (header.distribution == 1)
            header.distribution = 0;
        encryptor.encrypt(headerBuffer.data(), &header, sizeof(header), 0, 0x200);
        contentStorage->WritePlaceholder(placeholderId, 0, headerBuffer.data(), headerBuffer.size());
        headerFlushed = true;
    }

    NcmContentId contentId {};
    NcmPlaceHolderId placeholderId {};
    std::shared_ptr<nxncm::ContentStorage> contentStorage;
    std::vector<u8> headerBuffer;
    std::shared_ptr<NcaBodyWriter> writer;
    bool headerFlushed = false;
    bool closed = false;
};

struct StreamProgressContext {
    const InstallBackendCallbacks* callbacks = nullptr;
    uint64_t packageCompletedBytes = 0;
    uint64_t packageTotalBytes = 1;
    uint64_t totalCompletedBytesBefore = 0;
    uint64_t totalBytes = 1;
};

StreamProgressContext gStreamProgress {};

NcmPlaceHolderId contentIdToPlaceholderId(const NcmContentId& contentId) {
    NcmPlaceHolderId placeholderId {};
    static_assert(sizeof(placeholderId) == sizeof(contentId), "Unexpected content/placeholder id size mismatch");
    std::memcpy(&placeholderId, &contentId, sizeof(placeholderId));
    return placeholderId;
}

bool isRemotePath(const std::string& path) {
    if (path.rfind("web:", 0) == 0)
        return true;

    auto schemePos = path.find(":/");
    if (schemePos == std::string::npos)
        return false;

    return path.compare(0, schemePos + 2, "sdmc:/") != 0;
}

void emitLog(const InstallBackendCallbacks& callbacks, const std::string& line) {
#ifdef XXPLORE_DEBUG
    std::printf("[install-log] %s\n", line.c_str());
#endif
    if (callbacks.onLog)
        callbacks.onLog(line);
}

void emitStatus(const InstallBackendCallbacks& callbacks, const std::string& status) {
#ifdef XXPLORE_DEBUG
    std::printf("[install-status] %s\n", status.c_str());
#endif
    if (callbacks.onStatus)
        callbacks.onStatus(status);
}

void emitProgress(StreamProgressContext& ctx, uint64_t currentItemBytes, uint64_t currentItemTotal) {
    if (!ctx.callbacks)
        return;

    uint64_t currentDone = ctx.packageCompletedBytes + currentItemBytes;
    uint64_t totalDone = ctx.totalCompletedBytesBefore + currentDone;
    if (ctx.callbacks->onProgressBytes) {
        ctx.callbacks->onProgressBytes(currentDone, currentItemTotal,
                                       totalDone, ctx.totalBytes);
    }
    if (!ctx.callbacks->onProgress)
        return;

    float current = ctx.packageTotalBytes == 0 ? 0.0f :
        static_cast<float>(currentDone) / static_cast<float>(ctx.packageTotalBytes);
    float total = ctx.totalBytes == 0 ? 0.0f :
        static_cast<float>(totalDone) /
        static_cast<float>(ctx.totalBytes);
    if (current > 1.0f) current = 1.0f;
    if (total > 1.0f) total = 1.0f;
    ctx.callbacks->onProgress(current, total);
}

void beginContainerEntry(const std::string& name, uint64_t entrySize) {
    if (gStreamProgress.callbacks) {
        emitStatus(*gStreamProgress.callbacks, name);
        emitLog(*gStreamProgress.callbacks, "Installing " + name);
        emitProgress(gStreamProgress, 0, entrySize);
    }
}

void updateContainerEntry(uint64_t entryBytes, uint64_t entrySize) {
    emitProgress(gStreamProgress, entryBytes, entrySize);
}

void finishContainerEntry(uint64_t entrySize) {
    gStreamProgress.packageCompletedBytes += entrySize;
    emitProgress(gStreamProgress, 0, 1);
}

constexpr size_t kInstallReadChunkSize = 0x100000;

size_t installReadAheadBlocks() {
    return appletGetAppletType() == AppletType_LibraryApplet ? 4U : 8U;
}

class BufferedInstallReader {
public:
    using ReadFn = std::function<bool(uint64_t offset, size_t size, void* outBuffer,
                                      std::string& errOut)>;

    BufferedInstallReader(std::string debugPath, ReadFn readFn)
        : debugPath_(std::move(debugPath)), readFn_(std::move(readFn)) {
        chunkSize_ = kInstallReadChunkSize;
        maxBlocks_ = installReadAheadBlocks();
        if (maxBlocks_ == 0)
            maxBlocks_ = 1;
    }

    ~BufferedInstallReader() { stop(); }

    size_t chunkSize() const { return chunkSize_; }

    bool start(uint64_t startOffset, uint64_t totalBytes, std::string& errOut) {
        if (!readFn_) {
            errOut = "Install data source unavailable";
            return false;
        }

        stop();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            startOffset_ = startOffset;
            totalBytes_ = totalBytes;
            nextReadOffset_ = startOffset;
            bytesScheduled_ = 0;
            bytesConsumed_ = 0;
            stopRequested_ = false;
            producerDone_ = false;
            producerError_.clear();
            queue_.clear();
        }

        debugPrint("pipeline", "start path=%s chunk=%zu buffers=%zu total=%llu",
                   debugPath_.c_str(), chunkSize_, maxBlocks_,
                   static_cast<unsigned long long>(totalBytes));
        if (maxBlocks_ <= 1)
            return true;
        worker_ = std::thread([this]() { workerLoop(); });
        return true;
    }

    bool readNext(void* outBuffer, size_t size, std::string& errOut) {
        if (!readFn_) {
            errOut = "Install data source unavailable";
            return false;
        }

        if (maxBlocks_ <= 1) {
            bool ok = readFn_(startOffset_ + bytesConsumed_, size, outBuffer, errOut);
            if (ok)
                bytesConsumed_ += size;
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
                errOut = "Remote prefetch ended unexpectedly";
                return false;
            }

            chunk = std::move(queue_.front());
            queue_.pop_front();
            bytesConsumed_ += chunk.data.size();
            cvNotFull_.notify_one();
        }

        if (!chunk.error.empty()) {
            errOut = chunk.error;
            return false;
        }
        if (chunk.data.size() != size) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "Remote chunk size mismatch: expected %zu got %zu",
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
        std::vector<u8> data;
        std::string error;
    };

    void workerLoop() {
        for (;;) {
            uint64_t offset = 0;
            size_t size = 0;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cvNotFull_.wait(lock, [this]() {
                    return stopRequested_ || queue_.size() < maxBlocks_;
                });

                if (stopRequested_)
                    break;
                if (bytesScheduled_ >= totalBytes_) {
                    producerDone_ = true;
                    cvNotEmpty_.notify_all();
                    break;
                }

                offset = nextReadOffset_;
                uint64_t remaining = totalBytes_ - bytesScheduled_;
                size = static_cast<size_t>(std::min<uint64_t>(chunkSize_, remaining));
                nextReadOffset_ += size;
                bytesScheduled_ += size;
            }

            Chunk chunk;
            chunk.data.resize(size);
            std::string err;
            if (!readFn_(offset, size, chunk.data.data(), err)) {
                chunk.error = err.empty() ? "Install read failed" : err;
            }

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

    std::string debugPath_;
    ReadFn readFn_;
    size_t chunkSize_ = 0x100000;
    size_t maxBlocks_ = 1;

    uint64_t startOffset_ = 0;
    uint64_t totalBytes_ = 0;
    uint64_t nextReadOffset_ = 0;
    uint64_t bytesScheduled_ = 0;
    uint64_t bytesConsumed_ = 0;

    bool stopRequested_ = false;
    bool producerDone_ = false;
    std::string producerError_;
    std::deque<Chunk> queue_;
    std::mutex mutex_;
    std::condition_variable cvNotEmpty_;
    std::condition_variable cvNotFull_;
    std::thread worker_;
};

class InstallTask {
public:
    InstallTask(NcmStorageId storageId, bool ignoreReqFirmVersion)
        : destStorageId(storageId), ignoreReqFirmVersion(ignoreReqFirmVersion) {}

    virtual ~InstallTask() = default;

    void Prepare() {
        auto cnmtList = ReadCNMT();
        for (size_t i = 0; i < cnmtList.size(); i++) {
            auto [contentMeta, cnmtContentInfo] = cnmtList[i];
            contentMetas.push_back(contentMeta);
            nxncm::ContentStorage contentStorage(destStorageId);
            if (!contentStorage.Has(cnmtContentInfo.content_id))
                InstallNCA(cnmtContentInfo.content_id);
            data::ByteBuffer installContentMetaBuf;
            contentMetas[i].GetInstallContentMeta(installContentMetaBuf, cnmtContentInfo, ignoreReqFirmVersion);
            InstallContentMetaRecords(installContentMetaBuf, static_cast<int>(i));
            InstallApplicationRecord(static_cast<int>(i));
        }
    }

    void Begin() {
        try {
            InstallTicketCert();
        } catch (const std::runtime_error& e) {
            LOG_DEBUG("Ticket install warning: %s\n", e.what());
        }
        for (auto& contentMeta : contentMetas) {
            for (auto& record : contentMeta.GetContentInfos())
                InstallNCA(record.content_id);
        }
    }

protected:
    virtual std::vector<std::tuple<nxncm::ContentMeta, NcmContentInfo>> ReadCNMT() = 0;
    virtual void InstallTicketCert() = 0;
    virtual void InstallNCA(const NcmContentId& ncaId) = 0;

    void InstallContentMetaRecords(data::ByteBuffer& installContentMetaBuf, int index) {
        NcmContentMetaDatabase database {};
        NcmContentMetaKey key = contentMetas[index].GetContentMetaKey();
        XP_ASSERT_OK(ncmOpenContentMetaDatabase(&database, destStorageId), "Failed to open content meta database");
        XP_ASSERT_OK(ncmContentMetaDatabaseSet(&database, &key,
                                              reinterpret_cast<NcmContentMetaHeader*>(installContentMetaBuf.GetData()),
                                              installContentMetaBuf.GetSize()),
                     "Failed to set content records");
        XP_ASSERT_OK(ncmContentMetaDatabaseCommit(&database), "Failed to commit content records");
        serviceClose(&database.s);
    }

    void InstallApplicationRecord(int index) {
        u64 baseTitleId = util::GetBaseTitleId(contentMetas[index].GetContentMetaKey().id,
                                               static_cast<NcmContentMetaType>(contentMetas[index].GetContentMetaKey().type));
        ipc::ContentStorageRecord storageRecord {};
        storageRecord.metaRecord = contentMetas[index].GetContentMetaKey();
        storageRecord.storageId = destStorageId;
        XP_ASSERT_OK(ipc::nsPushApplicationRecord(baseTitleId, ipc::NsApplicationRecordType_Installed,
                                                  &storageRecord, 1),
                     "Failed to push application record");
    }

    const NcmStorageId destStorageId;
    bool ignoreReqFirmVersion;
    std::vector<nxncm::ContentMeta> contentMetas;
};

class SDMCNSP final : public install::nsp::NSP {
public:
    explicit SDMCNSP(std::string path) : filePath(std::move(path)) {
        file = std::fopen(filePath.c_str(), "rb");
        if (!file)
            XP_THROW("Failed to open NSP");
    }

    ~SDMCNSP() override {
        if (file)
            std::fclose(file);
    }

    void StreamToPlaceholder(std::shared_ptr<nxncm::ContentStorage>& contentStorage,
                             const NcmPlaceHolderId& placeholderId,
                             const NcmContentId& ncaId) override {
        const PFS0FileEntry* fileEntry = GetFileEntryByNcaId(ncaId);
        if (!fileEntry)
            XP_THROW("NCA entry not found in NSP");
        std::string ncaFileName = GetFileEntryName(fileEntry);
        size_t ncaSize = fileEntry->fileSize;
        debugPrint("install", "stream nsp entry=%s size=%llu", ncaFileName.c_str(),
                   static_cast<unsigned long long>(ncaSize));

        NcaWriter writer(ncaId, placeholderId, contentStorage);
        u64 fileStart = GetDataOffset() + fileEntry->dataOffset;
        u64 fileOff = 0;
        BufferedInstallReader reader(filePath, [this](uint64_t offset, size_t size,
                                                      void* outBuffer, std::string& errOut) {
            ::fseeko(file, static_cast<off_t>(offset), SEEK_SET);
            size_t n = std::fread(outBuffer, 1, size, file);
            if (n != size) {
                errOut = "Local read failed";
                return false;
            }
            return true;
        });
        size_t readSize = reader.chunkSize();
        auto buffer = std::make_unique<u8[]>(readSize);
        std::string readerErr;
        if (!reader.start(fileStart, ncaSize, readerErr))
            XP_THROW(readerErr);

        beginContainerEntry(ncaFileName, ncaSize);
        while (fileOff < ncaSize) {
            size_t chunk = readSize;
            if (fileOff + chunk >= ncaSize)
                chunk = static_cast<size_t>(ncaSize - fileOff);
            std::string err;
            if (!reader.readNext(buffer.get(), chunk, err))
                XP_THROW(err.empty() ? "Install read failed" : err);
            writer.write(buffer.get(), chunk);
            fileOff += chunk;
            updateContainerEntry(fileOff, ncaSize);
        }
        reader.stop();
        writer.close();
        finishContainerEntry(ncaSize);
    }

    void BufferData(void* buf, off_t offset, size_t size) override {
        ::fseeko(file, offset, SEEK_SET);
        std::fread(buf, 1, size, file);
    }

    uint64_t CalculateWorkBytes() const {
        uint64_t total = 0;
        const char* exts[] = { "nca", "cnmt.nca", "ncz", "cnmt.ncz" };
        for (const char* ext : exts) {
            for (const auto* entry : GetFileEntriesByExtension(ext))
                total += entry->fileSize;
        }
        return total == 0 ? 1 : total;
    }

private:
    std::string filePath;
    FILE* file = nullptr;
};

class SDMCXCI final : public install::xci::XCI {
public:
    explicit SDMCXCI(std::string path) : filePath(std::move(path)) {
        file = std::fopen(filePath.c_str(), "rb");
        if (!file)
            XP_THROW("Failed to open XCI");
    }

    ~SDMCXCI() override {
        if (file)
            std::fclose(file);
    }

    void StreamToPlaceholder(std::shared_ptr<nxncm::ContentStorage>& contentStorage,
                             const NcmPlaceHolderId& placeholderId,
                             const NcmContentId& ncaId) override {
        const HFS0FileEntry* fileEntry = GetFileEntryByNcaId(ncaId);
        if (!fileEntry)
            XP_THROW("NCA entry not found in XCI");
        std::string ncaFileName = GetFileEntryName(fileEntry);
        size_t ncaSize = fileEntry->fileSize;
        debugPrint("install", "stream xci entry=%s size=%llu", ncaFileName.c_str(),
                   static_cast<unsigned long long>(ncaSize));

        NcaWriter writer(ncaId, placeholderId, contentStorage);
        u64 fileStart = GetDataOffset() + fileEntry->dataOffset;
        u64 fileOff = 0;
        BufferedInstallReader reader(filePath, [this](uint64_t offset, size_t size,
                                                      void* outBuffer, std::string& errOut) {
            ::fseeko(file, static_cast<off_t>(offset), SEEK_SET);
            size_t n = std::fread(outBuffer, 1, size, file);
            if (n != size) {
                errOut = "Local read failed";
                return false;
            }
            return true;
        });
        size_t readSize = reader.chunkSize();
        auto buffer = std::make_unique<u8[]>(readSize);
        std::string readerErr;
        if (!reader.start(fileStart, ncaSize, readerErr))
            XP_THROW(readerErr);

        beginContainerEntry(ncaFileName, ncaSize);
        while (fileOff < ncaSize) {
            size_t chunk = readSize;
            if (fileOff + chunk >= ncaSize)
                chunk = static_cast<size_t>(ncaSize - fileOff);
            std::string err;
            if (!reader.readNext(buffer.get(), chunk, err))
                XP_THROW(err.empty() ? "Install read failed" : err);
            writer.write(buffer.get(), chunk);
            fileOff += chunk;
            updateContainerEntry(fileOff, ncaSize);
        }
        reader.stop();
        writer.close();
        finishContainerEntry(ncaSize);
    }

    void BufferData(void* buf, off_t offset, size_t size) override {
        ::fseeko(file, offset, SEEK_SET);
        std::fread(buf, 1, size, file);
    }

    uint64_t CalculateWorkBytes() const {
        uint64_t total = 0;
        const char* exts[] = { "nca", "cnmt.nca", "ncz", "cnmt.ncz" };
        for (const char* ext : exts) {
            for (const auto* entry : GetFileEntriesByExtension(ext))
                total += entry->fileSize;
        }
        return total == 0 ? 1 : total;
    }

private:
    std::string filePath;
    FILE* file = nullptr;
};

class RemoteNSP final : public install::nsp::NSP {
public:
    RemoteNSP(InstallQueueItem item, const InstallDataSourceCallbacks* sourceCallbacks)
        : item_(std::move(item)), sourceCallbacks_(sourceCallbacks) {}

    void StreamToPlaceholder(std::shared_ptr<nxncm::ContentStorage>& contentStorage,
                             const NcmPlaceHolderId& placeholderId,
                             const NcmContentId& ncaId) override {
        const PFS0FileEntry* fileEntry = GetFileEntryByNcaId(ncaId);
        if (!fileEntry)
            XP_THROW("NCA entry not found in remote NSP");
        std::string ncaFileName = GetFileEntryName(fileEntry);
        size_t ncaSize = fileEntry->fileSize;
        debugPrint("install", "stream remote nsp entry=%s size=%llu", ncaFileName.c_str(),
                   static_cast<unsigned long long>(ncaSize));

        NcaWriter writer(ncaId, placeholderId, contentStorage);
        u64 fileStart = GetDataOffset() + fileEntry->dataOffset;
        u64 fileOff = 0;
        BufferedInstallReader reader(item_.path,
                                     [this](uint64_t offset, size_t size, void* outBuffer,
                                            std::string& errOut) {
                                         if (!sourceCallbacks_ || !sourceCallbacks_->readRange) {
                                             errOut = "Remote install data source unavailable";
                                             return false;
                                         }
                                         return sourceCallbacks_->readRange(item_, offset, size,
                                                                            outBuffer, errOut);
                                     });
        size_t readSize = reader.chunkSize();
        auto buffer = std::make_unique<u8[]>(readSize);
        std::string readerErr;
        if (!reader.start(fileStart, ncaSize, readerErr))
            XP_THROW(readerErr);

        beginContainerEntry(ncaFileName, ncaSize);
        while (fileOff < ncaSize) {
            size_t chunk = readSize;
            if (fileOff + chunk >= ncaSize)
                chunk = static_cast<size_t>(ncaSize - fileOff);
            std::string err;
            if (!reader.readNext(buffer.get(), chunk, err))
                XP_THROW(err.empty() ? "Remote read failed" : err);
            writer.write(buffer.get(), chunk);
            fileOff += chunk;
            updateContainerEntry(fileOff, ncaSize);
        }
        reader.stop();
        writer.close();
        finishContainerEntry(ncaSize);
    }

    void BufferData(void* buf, off_t offset, size_t size) override {
        if (!sourceCallbacks_ || !sourceCallbacks_->readRange)
            XP_THROW("Remote install data source unavailable");
        std::string err;
        if (!sourceCallbacks_->readRange(item_, static_cast<uint64_t>(offset), size, buf, err))
            XP_THROW(err.empty() ? "Remote read failed" : err);
    }

private:
    InstallQueueItem item_;
    const InstallDataSourceCallbacks* sourceCallbacks_ = nullptr;
};

class RemoteXCI final : public install::xci::XCI {
public:
    RemoteXCI(InstallQueueItem item, const InstallDataSourceCallbacks* sourceCallbacks)
        : item_(std::move(item)), sourceCallbacks_(sourceCallbacks) {}

    void StreamToPlaceholder(std::shared_ptr<nxncm::ContentStorage>& contentStorage,
                             const NcmPlaceHolderId& placeholderId,
                             const NcmContentId& ncaId) override {
        const HFS0FileEntry* fileEntry = GetFileEntryByNcaId(ncaId);
        if (!fileEntry)
            XP_THROW("NCA entry not found in remote XCI");
        std::string ncaFileName = GetFileEntryName(fileEntry);
        size_t ncaSize = fileEntry->fileSize;
        debugPrint("install", "stream remote xci entry=%s size=%llu", ncaFileName.c_str(),
                   static_cast<unsigned long long>(ncaSize));

        NcaWriter writer(ncaId, placeholderId, contentStorage);
        u64 fileStart = GetDataOffset() + fileEntry->dataOffset;
        u64 fileOff = 0;
        BufferedInstallReader reader(item_.path,
                                     [this](uint64_t offset, size_t size, void* outBuffer,
                                            std::string& errOut) {
                                         if (!sourceCallbacks_ || !sourceCallbacks_->readRange) {
                                             errOut = "Remote install data source unavailable";
                                             return false;
                                         }
                                         return sourceCallbacks_->readRange(item_, offset, size,
                                                                            outBuffer, errOut);
                                     });
        size_t readSize = reader.chunkSize();
        auto buffer = std::make_unique<u8[]>(readSize);
        std::string readerErr;
        if (!reader.start(fileStart, ncaSize, readerErr))
            XP_THROW(readerErr);

        beginContainerEntry(ncaFileName, ncaSize);
        while (fileOff < ncaSize) {
            size_t chunk = readSize;
            if (fileOff + chunk >= ncaSize)
                chunk = static_cast<size_t>(ncaSize - fileOff);
            std::string err;
            if (!reader.readNext(buffer.get(), chunk, err))
                XP_THROW(err.empty() ? "Remote read failed" : err);
            writer.write(buffer.get(), chunk);
            fileOff += chunk;
            updateContainerEntry(fileOff, ncaSize);
        }
        reader.stop();
        writer.close();
        finishContainerEntry(ncaSize);
    }

    void BufferData(void* buf, off_t offset, size_t size) override {
        if (!sourceCallbacks_ || !sourceCallbacks_->readRange)
            XP_THROW("Remote install data source unavailable");
        std::string err;
        if (!sourceCallbacks_->readRange(item_, static_cast<uint64_t>(offset), size, buf, err))
            XP_THROW(err.empty() ? "Remote read failed" : err);
    }

private:
    InstallQueueItem item_;
    const InstallDataSourceCallbacks* sourceCallbacks_ = nullptr;
};

class NSPInstallTask final : public InstallTask {
public:
    NSPInstallTask(NcmStorageId storageId, std::shared_ptr<install::nsp::NSP> nsp)
        : InstallTask(storageId, true), package(std::move(nsp)) {
        package->RetrieveHeader();
    }

protected:
    std::vector<std::tuple<nxncm::ContentMeta, NcmContentInfo>> ReadCNMT() override {
        std::vector<std::tuple<nxncm::ContentMeta, NcmContentInfo>> list;
        for (const auto* fileEntry : package->GetFileEntriesByExtension("cnmt.nca")) {
            std::string cnmtName = package->GetFileEntryName(fileEntry);
            NcmContentId cnmtId = util::GetNcaIdFromString(cnmtName);
            size_t cnmtSize = fileEntry->fileSize;
            InstallNCA(cnmtId);
            nxncm::ContentStorage contentStorage(destStorageId);
            if (!contentStorage.Has(cnmtId))
                XP_THROW("CNMT NCA was not registered after install");
            std::string installedPath = contentStorage.GetPath(cnmtId);
            NcmContentInfo info {};
            info.content_id = cnmtId;
            ncmU64ToContentInfoSize(cnmtSize & 0xFFFFFFFFFFFFULL, &info);
            info.content_type = NcmContentType_Meta;
            list.push_back({ install::nsp::GetContentMetaFromNCA(installedPath), info });
        }
        return list;
    }

    void InstallNCA(const NcmContentId& ncaId) override {
        const auto* fileEntry = package->GetFileEntryByNcaId(ncaId);
        if (!fileEntry)
            XP_THROW("NCA file not found in NSP");

        auto storage = std::make_shared<nxncm::ContentStorage>(destStorageId);
        NcmPlaceHolderId stalePlaceholderId = contentIdToPlaceholderId(ncaId);
        try {
            if (storage->HasPlaceholder(stalePlaceholderId)) {
                debugPrint("install", "cleanup stale placeholder for %s",
                           util::GetNcaIdString(ncaId).c_str());
                storage->DeletePlaceholder(stalePlaceholderId);
            }
        } catch (...) {}

        NcmPlaceHolderId placeholderId = storage->GeneratePlaceholderId();
        debugPrint("install", "generated placeholder for %s", util::GetNcaIdString(ncaId).c_str());
        try {
            package->StreamToPlaceholder(storage, placeholderId, ncaId);
            debugPrint("install", "placeholder exists before register=%d",
                       storage->HasPlaceholder(placeholderId) ? 1 : 0);
            storage->Register(placeholderId, ncaId);
        } catch (...) {
            bool contentPresent = false;
            debugPrint("install", "register failed content=%s placeholder_exists=%d",
                       util::GetNcaIdString(ncaId).c_str(),
                       storage->HasPlaceholder(placeholderId) ? 1 : 0);
            try {
                contentPresent = storage->Has(ncaId);
                if (gStreamProgress.callbacks && contentPresent)
                    emitLog(*gStreamProgress.callbacks, "Content already present: " + util::GetNcaIdString(ncaId));
            } catch (...) {}
            try {
                if (storage->HasPlaceholder(placeholderId))
                    storage->DeletePlaceholder(placeholderId);
            } catch (...) {}
            if (!contentPresent)
                throw;
        }
        try { storage->DeletePlaceholder(placeholderId); } catch (...) {}
    }

    void InstallTicketCert() override {
        auto tikEntries = package->GetFileEntriesByExtension("tik");
        auto certEntries = package->GetFileEntriesByExtension("cert");
        if (tikEntries.size() != certEntries.size())
            XP_THROW("Ticket/cert mismatch");

        for (size_t i = 0; i < tikEntries.size(); i++) {
            auto tikSize = tikEntries[i]->fileSize;
            auto certSize = certEntries[i]->fileSize;
            auto tikBuf = std::make_unique<u8[]>(tikSize);
            auto certBuf = std::make_unique<u8[]>(certSize);
            package->BufferData(tikBuf.get(), package->GetDataOffset() + tikEntries[i]->dataOffset, tikSize);
            package->BufferData(certBuf.get(), package->GetDataOffset() + certEntries[i]->dataOffset, certSize);
            XP_ASSERT_OK(ipc::esImportTicket(tikBuf.get(), tikSize, certBuf.get(), certSize),
                         "Failed to import ticket");
        }
    }

private:
    std::shared_ptr<install::nsp::NSP> package;
};

class XCIInstallTask final : public InstallTask {
public:
    XCIInstallTask(NcmStorageId storageId, std::shared_ptr<install::xci::XCI> xci)
        : InstallTask(storageId, true), package(std::move(xci)) {
        package->RetrieveHeader();
    }

protected:
    std::vector<std::tuple<nxncm::ContentMeta, NcmContentInfo>> ReadCNMT() override {
        std::vector<std::tuple<nxncm::ContentMeta, NcmContentInfo>> list;
        for (const auto* fileEntry : package->GetFileEntriesByExtension("cnmt.nca")) {
            std::string cnmtName = package->GetFileEntryName(fileEntry);
            NcmContentId cnmtId = util::GetNcaIdFromString(cnmtName);
            size_t cnmtSize = fileEntry->fileSize;
            InstallNCA(cnmtId);
            nxncm::ContentStorage contentStorage(destStorageId);
            if (!contentStorage.Has(cnmtId))
                XP_THROW("CNMT NCA was not registered after install");
            std::string installedPath = contentStorage.GetPath(cnmtId);
            NcmContentInfo info {};
            info.content_id = cnmtId;
            ncmU64ToContentInfoSize(cnmtSize & 0xFFFFFFFFFFFFULL, &info);
            info.content_type = NcmContentType_Meta;
            list.push_back({ install::nsp::GetContentMetaFromNCA(installedPath), info });
        }
        return list;
    }

    void InstallNCA(const NcmContentId& ncaId) override {
        const auto* fileEntry = package->GetFileEntryByNcaId(ncaId);
        if (!fileEntry)
            XP_THROW("NCA file not found in XCI");

        auto storage = std::make_shared<nxncm::ContentStorage>(destStorageId);
        NcmPlaceHolderId stalePlaceholderId = contentIdToPlaceholderId(ncaId);
        try {
            if (storage->HasPlaceholder(stalePlaceholderId)) {
                debugPrint("install", "cleanup stale placeholder for %s",
                           util::GetNcaIdString(ncaId).c_str());
                storage->DeletePlaceholder(stalePlaceholderId);
            }
        } catch (...) {}

        NcmPlaceHolderId placeholderId = storage->GeneratePlaceholderId();
        debugPrint("install", "generated placeholder for %s", util::GetNcaIdString(ncaId).c_str());
        try {
            package->StreamToPlaceholder(storage, placeholderId, ncaId);
            debugPrint("install", "placeholder exists before register=%d",
                       storage->HasPlaceholder(placeholderId) ? 1 : 0);
            storage->Register(placeholderId, ncaId);
        } catch (...) {
            bool contentPresent = false;
            debugPrint("install", "register failed content=%s placeholder_exists=%d",
                       util::GetNcaIdString(ncaId).c_str(),
                       storage->HasPlaceholder(placeholderId) ? 1 : 0);
            try {
                contentPresent = storage->Has(ncaId);
                if (gStreamProgress.callbacks && contentPresent)
                    emitLog(*gStreamProgress.callbacks, "Content already present: " + util::GetNcaIdString(ncaId));
            } catch (...) {}
            try {
                if (storage->HasPlaceholder(placeholderId))
                    storage->DeletePlaceholder(placeholderId);
            } catch (...) {}
            if (!contentPresent)
                throw;
        }
        try { storage->DeletePlaceholder(placeholderId); } catch (...) {}
    }

    void InstallTicketCert() override {
        auto tikEntries = package->GetFileEntriesByExtension("tik");
        auto certEntries = package->GetFileEntriesByExtension("cert");
        if (tikEntries.size() != certEntries.size())
            XP_THROW("Ticket/cert mismatch");

        for (size_t i = 0; i < tikEntries.size(); i++) {
            auto tikSize = tikEntries[i]->fileSize;
            auto certSize = certEntries[i]->fileSize;
            auto tikBuf = std::make_unique<u8[]>(tikSize);
            auto certBuf = std::make_unique<u8[]>(certSize);
            package->BufferData(tikBuf.get(), package->GetDataOffset() + tikEntries[i]->dataOffset, tikSize);
            package->BufferData(certBuf.get(), package->GetDataOffset() + certEntries[i]->dataOffset, certSize);
            XP_ASSERT_OK(ipc::esImportTicket(tikBuf.get(), tikSize, certBuf.get(), certSize),
                         "Failed to import ticket");
        }
    }

private:
    std::shared_ptr<install::xci::XCI> package;
};

bool isXciPath(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos)
        return false;
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".xci" || ext == ".xcz";
}

uint64_t sumWorkBytes(const install::nsp::NSP& package) {
    uint64_t total = 0;
    const char* exts[] = { "nca", "cnmt.nca", "ncz", "cnmt.ncz" };
    for (const char* ext : exts)
        for (const auto* entry : package.GetFileEntriesByExtension(ext))
            total += entry->fileSize;
    return total == 0 ? 1 : total;
}

uint64_t sumWorkBytes(const install::xci::XCI& package) {
    uint64_t total = 0;
    const char* exts[] = { "nca", "cnmt.nca", "ncz", "cnmt.ncz" };
    for (const char* ext : exts)
        for (const auto* entry : package.GetFileEntriesByExtension(ext))
            total += entry->fileSize;
    return total == 0 ? 1 : total;
}

} // namespace

bool runInstallQueue(const std::vector<InstallQueueItem>& items, bool installToNand,
                     bool deleteAfterInstall, const InstallBackendCallbacks& callbacks,
                     std::string& errOut, const InstallDataSourceCallbacks* sourceCallbacks) {
    if (items.empty()) {
        errOut = "No install items";
        return false;
    }

    uint64_t totalWorkBytes = 0;
    std::vector<uint64_t> perPackageWork(items.size(), 1);

    try {
        debugPrint("install", "queue start items=%zu target=%s delete_after=%d", items.size(),
                   installToNand ? "nand" : "sd", deleteAfterInstall ? 1 : 0);
        for (size_t i = 0; i < items.size(); i++) {
            const bool remote = isRemotePath(items[i].path);
            if (remote && (!sourceCallbacks || !sourceCallbacks->readRange))
                XP_THROW("Remote install requested without data source");

            if (isXciPath(items[i].path)) {
                std::shared_ptr<install::xci::XCI> package =
                    remote
                        ? std::static_pointer_cast<install::xci::XCI>(
                              std::make_shared<RemoteXCI>(items[i], sourceCallbacks))
                        : std::static_pointer_cast<install::xci::XCI>(
                              std::make_shared<SDMCXCI>(items[i].path));
                package->RetrieveHeader();
                perPackageWork[i] = sumWorkBytes(*package);
            } else {
                std::shared_ptr<install::nsp::NSP> package =
                    remote
                        ? std::static_pointer_cast<install::nsp::NSP>(
                              std::make_shared<RemoteNSP>(items[i], sourceCallbacks))
                        : std::static_pointer_cast<install::nsp::NSP>(
                              std::make_shared<SDMCNSP>(items[i].path));
                package->RetrieveHeader();
                perPackageWork[i] = sumWorkBytes(*package);
            }
            totalWorkBytes += perPackageWork[i];
        }
    } catch (const std::exception& e) {
        errOut = e.what();
        return false;
    }

    NcmStorageId storageId = installToNand ? NcmStorageId_BuiltInUser : NcmStorageId_SdCard;
    uint64_t completedBytes = 0;

    try {
        ncmInitialize();
        ipc::nsextInitialize();
        ipc::esInitialize();
        splCryptoInitialize();
        splInitialize();

        for (size_t i = 0; i < items.size(); i++) {
            debugPrint("install", "begin package name=%s path=%s", items[i].name.c_str(),
                       items[i].path.c_str());
            emitLog(callbacks, "Preparing " + items[i].name);
            emitStatus(callbacks, items[i].name);

            gStreamProgress.callbacks = &callbacks;
            gStreamProgress.packageCompletedBytes = 0;
            gStreamProgress.packageTotalBytes = perPackageWork[i];
            gStreamProgress.totalCompletedBytesBefore = completedBytes;
            gStreamProgress.totalBytes = totalWorkBytes == 0 ? 1 : totalWorkBytes;
            emitProgress(gStreamProgress, 0, 1);

            const bool remote = isRemotePath(items[i].path);
            if (isXciPath(items[i].path)) {
                std::shared_ptr<install::xci::XCI> package =
                    remote
                        ? std::static_pointer_cast<install::xci::XCI>(
                              std::make_shared<RemoteXCI>(items[i], sourceCallbacks))
                        : std::static_pointer_cast<install::xci::XCI>(
                              std::make_shared<SDMCXCI>(items[i].path));
                XCIInstallTask task(storageId, package);
                task.Prepare();
                task.Begin();
            } else {
                std::shared_ptr<install::nsp::NSP> package =
                    remote
                        ? std::static_pointer_cast<install::nsp::NSP>(
                              std::make_shared<RemoteNSP>(items[i], sourceCallbacks))
                        : std::static_pointer_cast<install::nsp::NSP>(
                              std::make_shared<SDMCNSP>(items[i].path));
                NSPInstallTask task(storageId, package);
                task.Prepare();
                task.Begin();
            }

            completedBytes += perPackageWork[i];
            gStreamProgress.packageCompletedBytes = perPackageWork[i];
            emitProgress(gStreamProgress, 0, 1);
            emitLog(callbacks, "Installed " + items[i].name);
            debugPrint("install", "package done name=%s", items[i].name.c_str());

            if (deleteAfterInstall) {
                if (remote)
                    continue;
                std::error_code ec;
                std::filesystem::remove(items[i].path, ec);
                if (ec)
                    emitLog(callbacks, "Delete failed: " + items[i].name + " (" + ec.message() + ")");
                else
                    emitLog(callbacks, "Deleted source: " + items[i].name);
            }
        }

        splExit();
        splCryptoExit();
        ipc::esExit();
        ipc::nsextExit();
        ncmExit();
        debugPrint("install", "queue finished success");
        return true;
    } catch (const std::exception& e) {
        errOut = e.what();
        if (callbacks.onLog)
            callbacks.onLog("Partially installed contents can be removed from System Settings.");
        debugPrint("install", "queue failed err=%s", errOut.c_str());
        splExit();
        splCryptoExit();
        ipc::esExit();
        ipc::nsextExit();
        ncmExit();
        return false;
    }
}

} // namespace xxplore
