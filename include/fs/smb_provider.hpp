#pragma once

#include "fs/file_provider.hpp"
#include <string>

// Forward declare libsmb2 types
struct smb2_context;

namespace xplore {
namespace fs {

/// SMB2 network file system provider using libsmb2.
class SmbProvider : public FileProvider {
public:
    /// @param id     Unique provider id (e.g. "smb-abc123")
    /// @param name   Display name (e.g. "MyPC")
    /// @param server Server address (e.g. "192.168.1.100")
    /// @param share  Share name (e.g. "shared")
    /// @param user   Username (may be empty for guest)
    /// @param pass   Password (may be empty)
    SmbProvider(std::string id, std::string name, std::string server,
                std::string share, std::string user, std::string pass);
    ~SmbProvider() override;

    std::string providerId() const override { return id_; }
    std::string displayPrefix() const override;
    bool isReadOnly() const override { return false; }
    bool allowsSelection() const override { return true; }

    std::vector<FileEntry> listDir(const std::string& path, std::string& errOut) override;
    bool statPath(const std::string& path, FileStatInfo& out, std::string& errOut) override;
    bool createDirectory(const std::string& path, std::string& errOut) override;
    bool removeAll(const std::string& path, std::string& errOut) override;
    bool renamePath(const std::string& from, const std::string& to,
                    std::string& errOut) override;
    bool readFile(const std::string& path, uint64_t offset, size_t size,
                  void* outBuffer, std::string& errOut) override;
    bool writeFile(const std::string& path, const void* data, size_t size,
                   std::string& errOut) override;

    /// Connect to the SMB2 share. Must be called before other operations.
    bool connect(std::string& errOut);

    /// Disconnect. Called on destruction too.
    void disconnect();

    /// Test connectivity.
    bool testConnection(std::string& errOut);

    bool isConnected() const { return connected_; }

private:
    bool shouldReconnect(const std::string& err) const;
    bool reconnect(std::string& errOut);

    std::string id_;
    std::string name_;
    std::string server_;
    std::string share_;
    std::string user_;
    std::string pass_;

    smb2_context* smb2_ = nullptr;
    bool connected_ = false;

    /// Ensure connected, returning error if not.
    bool ensureConnected(std::string& errOut);

    /// Convert relative path for SMB (forward slashes, no leading /)
    std::string smbPath(const std::string& relPath) const;

    /// Recursive delete helper
    bool removeRecursive(const std::string& smbRelPath, std::string& errOut);
};

} // namespace fs
} // namespace xplore
