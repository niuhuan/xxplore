#include "fs/smb_provider.hpp"

#include <algorithm>
#include <cstdarg>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

namespace xplore {
namespace fs {

namespace {

void debugLog(const char* fmt, ...) {
#ifdef XPLORE_DEBUG
    std::va_list args;
    va_start(args, fmt);
    std::printf("[smb] ");
    std::vprintf(fmt, args);
    std::printf("\n");
    va_end(args);
#else
    (void)fmt;
#endif
}

} // namespace

SmbProvider::SmbProvider(std::string id, std::string name, std::string server,
                         std::string share, std::string user, std::string pass)
    : id_(std::move(id)), name_(std::move(name)), server_(std::move(server)),
      share_(std::move(share)), user_(std::move(user)), pass_(std::move(pass)) {
}

SmbProvider::~SmbProvider() {
    disconnect();
}

std::string SmbProvider::displayPrefix() const {
    return name_ + "(SMB):";
}

std::string SmbProvider::smbPath(const std::string& relPath) const {
    // SMB paths use backslash, but libsmb2 accepts forward slashes
    // Remove leading / since smb2 paths are relative to share root
    std::string p = relPath;
    while (!p.empty() && p.front() == '/')
        p.erase(p.begin());
    return p;
}

bool SmbProvider::connect(std::string& errOut) {
    if (connected_)
        return true;

    smb2_ = smb2_init_context();
    if (!smb2_) {
        errOut = "smb2_init_context failed";
        return false;
    }

    smb2_set_security_mode(smb2_, SMB2_NEGOTIATE_SIGNING_ENABLED);

    if (!user_.empty()) {
        smb2_set_user(smb2_, user_.c_str());
        smb2_set_password(smb2_, pass_.c_str());
    }

    int rc = smb2_connect_share(smb2_, server_.c_str(), share_.c_str(),
                                 user_.empty() ? "Guest" : user_.c_str());
    if (rc < 0) {
        errOut = smb2_get_error(smb2_);
        debugLog("connect failed server=%s share=%s err=%s", server_.c_str(), share_.c_str(),
                 errOut.c_str());
        smb2_destroy_context(smb2_);
        smb2_ = nullptr;
        return false;
    }

    debugLog("connected server=%s share=%s user=%s", server_.c_str(), share_.c_str(),
             user_.empty() ? "Guest" : user_.c_str());
    connected_ = true;
    return true;
}

void SmbProvider::disconnect() {
    if (smb2_) {
        if (connected_)
            smb2_disconnect_share(smb2_);
        smb2_destroy_context(smb2_);
        smb2_ = nullptr;
        connected_ = false;
    }
}

bool SmbProvider::testConnection(std::string& errOut) {
    if (!connect(errOut))
        return false;
    // Try to stat root
    FileStatInfo info;
    return statPath("/", info, errOut);
}

bool SmbProvider::ensureConnected(std::string& errOut) {
    if (connected_)
        return true;
    return connect(errOut);
}

bool SmbProvider::shouldReconnect(const std::string& err) const {
    return err.find("POLLHUP") != std::string::npos ||
           err.find("POLLERR") != std::string::npos ||
           err.find("Transport endpoint is not connected") != std::string::npos ||
           err.find("Connection reset") != std::string::npos ||
           err.find("Broken pipe") != std::string::npos ||
           err.find("Socket is closed") != std::string::npos ||
           err.find("Connection closed") != std::string::npos;
}

bool SmbProvider::reconnect(std::string& errOut) {
    debugLog("reconnect requested server=%s share=%s", server_.c_str(), share_.c_str());
    disconnect();
    return connect(errOut);
}

// --- Public FileProvider API ---

std::vector<FileEntry> SmbProvider::listDir(const std::string& path, std::string& errOut) {
    std::string sp = smbPath(path);
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!ensureConnected(errOut))
            return {};

        debugLog("listDir rel=%s smb=%s attempt=%d", path.c_str(),
                 sp.empty() ? "<root>" : sp.c_str(), attempt + 1);
        struct smb2dir* dir = smb2_opendir(smb2_, sp.c_str());
        if (!dir) {
            errOut = smb2_get_error(smb2_);
            debugLog("listDir failed smb=%s err=%s", sp.empty() ? "<root>" : sp.c_str(),
                     errOut.c_str());
            if (attempt == 0 && shouldReconnect(errOut)) {
                if (!reconnect(errOut))
                    return {};
                continue;
            }
            return {};
        }

        std::vector<FileEntry> entries;
        struct smb2dirent* ent;
        while ((ent = smb2_readdir(smb2_, dir)) != nullptr) {
            if (std::strcmp(ent->name, ".") == 0 || std::strcmp(ent->name, "..") == 0)
                continue;

            FileEntry e;
            e.name = ent->name;
            e.isDirectory = (ent->st.smb2_type == SMB2_TYPE_DIRECTORY);
            e.size = ent->st.smb2_size;
            entries.push_back(std::move(e));
        }

        smb2_closedir(smb2_, dir);
        debugLog("listDir done smb=%s count=%zu", sp.empty() ? "<root>" : sp.c_str(), entries.size());

        std::sort(entries.begin(), entries.end(), [](const FileEntry& a, const FileEntry& b) {
            if (a.isDirectory != b.isDirectory) return a.isDirectory > b.isDirectory;
            return a.name < b.name;
        });

        return entries;
    }
    return {};
}

bool SmbProvider::statPath(const std::string& path, FileStatInfo& out, std::string& errOut) {
    std::string sp = smbPath(path);
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!ensureConnected(errOut))
            return false;

        debugLog("stat rel=%s smb=%s attempt=%d", path.c_str(),
                 sp.empty() ? "<root>" : sp.c_str(), attempt + 1);
        struct smb2_stat_64 st;
        int rc = smb2_stat(smb2_, sp.c_str(), &st);
        if (rc < 0) {
            const char* err = smb2_get_error(smb2_);
            if (err && (std::strstr(err, "STATUS_OBJECT_NAME_NOT_FOUND") ||
                        std::strstr(err, "Not found"))) {
                out.exists = false;
                return true;
            }
            errOut = err ? err : "stat failed";
            debugLog("stat failed smb=%s err=%s", sp.empty() ? "<root>" : sp.c_str(),
                     errOut.c_str());
            if (attempt == 0 && shouldReconnect(errOut)) {
                if (!reconnect(errOut))
                    return false;
                continue;
            }
            return false;
        }

        out.exists = true;
        out.isDirectory = (st.smb2_type == SMB2_TYPE_DIRECTORY);
        out.size = st.smb2_size;
        return true;
    }
    return false;
}

bool SmbProvider::createDirectory(const std::string& path, std::string& errOut) {
    std::string sp = smbPath(path);
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!ensureConnected(errOut))
            return false;
        int rc = smb2_mkdir(smb2_, sp.c_str());
        if (rc >= 0)
            return true;
        errOut = smb2_get_error(smb2_);
        if (attempt == 0 && shouldReconnect(errOut)) {
            if (!reconnect(errOut))
                return false;
            continue;
        }
        return false;
    }
    return false;
}

bool SmbProvider::removeRecursive(const std::string& smbRelPath, std::string& errOut) {
    struct smb2_stat_64 st;
    int rc = smb2_stat(smb2_, smbRelPath.c_str(), &st);
    if (rc < 0) {
        errOut = smb2_get_error(smb2_);
        return false;
    }

    if (st.smb2_type != SMB2_TYPE_DIRECTORY) {
        // File: unlink
        rc = smb2_unlink(smb2_, smbRelPath.c_str());
        if (rc < 0) {
            errOut = smb2_get_error(smb2_);
            return false;
        }
        return true;
    }

    // Directory: recurse
    struct smb2dir* dir = smb2_opendir(smb2_, smbRelPath.c_str());
    if (!dir) {
        errOut = smb2_get_error(smb2_);
        return false;
    }

    struct smb2dirent* ent;
    while ((ent = smb2_readdir(smb2_, dir)) != nullptr) {
        if (std::strcmp(ent->name, ".") == 0 || std::strcmp(ent->name, "..") == 0)
            continue;

        std::string childPath = smbRelPath;
        if (!childPath.empty() && childPath.back() != '/')
            childPath += '/';
        childPath += ent->name;

        if (!removeRecursive(childPath, errOut)) {
            smb2_closedir(smb2_, dir);
            return false;
        }
    }
    smb2_closedir(smb2_, dir);

    rc = smb2_rmdir(smb2_, smbRelPath.c_str());
    if (rc < 0) {
        errOut = smb2_get_error(smb2_);
        return false;
    }
    return true;
}

bool SmbProvider::removeAll(const std::string& path, std::string& errOut) {
    if (!ensureConnected(errOut))
        return false;
    return removeRecursive(smbPath(path), errOut);
}

bool SmbProvider::renamePath(const std::string& from, const std::string& to,
                              std::string& errOut) {
    std::string spFrom = smbPath(from);
    std::string spTo = smbPath(to);
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!ensureConnected(errOut))
            return false;
        int rc = smb2_rename(smb2_, spFrom.c_str(), spTo.c_str());
        if (rc >= 0)
            return true;
        errOut = smb2_get_error(smb2_);
        if (attempt == 0 && shouldReconnect(errOut)) {
            if (!reconnect(errOut))
                return false;
            continue;
        }
        return false;
    }
    return false;
}

bool SmbProvider::readFile(const std::string& path, uint64_t offset, size_t size,
                            void* outBuffer, std::string& errOut) {
    std::string sp = smbPath(path);
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!ensureConnected(errOut))
            return false;

        struct smb2fh* fh = smb2_open(smb2_, sp.c_str(), O_RDONLY);
        if (!fh) {
            errOut = smb2_get_error(smb2_);
            if (attempt == 0 && shouldReconnect(errOut)) {
                if (!reconnect(errOut))
                    return false;
                continue;
            }
            return false;
        }

        if (offset > 0) {
            int64_t seekResult = smb2_lseek(smb2_, fh, static_cast<int64_t>(offset), SEEK_SET, nullptr);
            if (seekResult < 0) {
                errOut = smb2_get_error(smb2_);
                smb2_close(smb2_, fh);
                if (attempt == 0 && shouldReconnect(errOut)) {
                    if (!reconnect(errOut))
                        return false;
                    continue;
                }
                return false;
            }
        }

        size_t totalRead = 0;
        auto* buf = static_cast<uint8_t*>(outBuffer);
        while (totalRead < size) {
            size_t chunk = std::min(size - totalRead, static_cast<size_t>(256 * 1024));
            int n = smb2_read(smb2_, fh, buf + totalRead, static_cast<uint32_t>(chunk));
            if (n < 0) {
                errOut = smb2_get_error(smb2_);
                smb2_close(smb2_, fh);
                if (attempt == 0 && shouldReconnect(errOut)) {
                    if (!reconnect(errOut))
                        return false;
                    goto retry_read;
                }
                return false;
            }
            if (n == 0) break;
            totalRead += static_cast<size_t>(n);
        }

        smb2_close(smb2_, fh);
        return true;
retry_read:
        continue;
    }
    return false;
}

bool SmbProvider::writeFile(const std::string& path, const void* data, size_t size,
                             std::string& errOut) {
    std::string sp = smbPath(path);
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!ensureConnected(errOut))
            return false;

        struct smb2fh* fh = smb2_open(smb2_, sp.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
        if (!fh) {
            errOut = smb2_get_error(smb2_);
            if (attempt == 0 && shouldReconnect(errOut)) {
                if (!reconnect(errOut))
                    return false;
                continue;
            }
            return false;
        }

        size_t totalWritten = 0;
        const auto* buf = static_cast<const uint8_t*>(data);
        while (totalWritten < size) {
            size_t chunk = std::min(size - totalWritten, static_cast<size_t>(256 * 1024));
            int n = smb2_write(smb2_, fh, buf + totalWritten, static_cast<uint32_t>(chunk));
            if (n < 0) {
                errOut = smb2_get_error(smb2_);
                smb2_close(smb2_, fh);
                if (attempt == 0 && shouldReconnect(errOut)) {
                    if (!reconnect(errOut))
                        return false;
                    goto retry_write;
                }
                return false;
            }
            totalWritten += static_cast<size_t>(n);
        }

        smb2_close(smb2_, fh);
        return true;
retry_write:
        continue;
    }
    return false;
}

bool SmbProvider::writeFileChunk(const std::string& path, uint64_t offset,
                                 const void* data, size_t size, bool truncate,
                                 std::string& errOut) {
    std::string sp = smbPath(path);
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!ensureConnected(errOut))
            return false;

        int flags = O_WRONLY | O_CREAT | (truncate ? O_TRUNC : 0);
        struct smb2fh* fh = smb2_open(smb2_, sp.c_str(), flags);
        if (!fh) {
            errOut = smb2_get_error(smb2_);
            if (attempt == 0 && shouldReconnect(errOut)) {
                if (!reconnect(errOut))
                    return false;
                continue;
            }
            return false;
        }

        if (offset > 0) {
            int64_t seekResult = smb2_lseek(smb2_, fh, static_cast<int64_t>(offset), SEEK_SET, nullptr);
            if (seekResult < 0) {
                errOut = smb2_get_error(smb2_);
                smb2_close(smb2_, fh);
                if (attempt == 0 && shouldReconnect(errOut)) {
                    if (!reconnect(errOut))
                        return false;
                    continue;
                }
                return false;
            }
        }

        size_t totalWritten = 0;
        const auto* buf = static_cast<const uint8_t*>(data);
        while (totalWritten < size) {
            size_t chunk = std::min(size - totalWritten, static_cast<size_t>(256 * 1024));
            int n = smb2_write(smb2_, fh, buf + totalWritten, static_cast<uint32_t>(chunk));
            if (n < 0) {
                errOut = smb2_get_error(smb2_);
                smb2_close(smb2_, fh);
                if (attempt == 0 && shouldReconnect(errOut)) {
                    if (!reconnect(errOut))
                        return false;
                    goto retry_write_chunk;
                }
                return false;
            }
            if (n == 0) {
                errOut = "smb2_write returned 0";
                smb2_close(smb2_, fh);
                return false;
            }
            totalWritten += static_cast<size_t>(n);
        }

        smb2_close(smb2_, fh);
        return true;
retry_write_chunk:
        continue;
    }
    return false;
}

} // namespace fs
} // namespace xplore
