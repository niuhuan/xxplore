#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <usbhsfs.h>
#include <vector>

namespace xxplore::fs {

struct UsbDriveInfo {
    std::string providerId;
    std::string mountName;
    bool        readOnly = false;
};

class UsbDriveManager {
public:
    UsbDriveManager() = default;
    ~UsbDriveManager();

    bool init(std::string& errOut);
    void shutdown();

    std::vector<UsbDriveInfo> snapshot() const;
    bool consumeChanged(std::vector<UsbDriveInfo>& out);
    std::vector<UsbDriveInfo> refreshNow();
    bool unmountByProviderId(const std::string& providerId, std::string& errOut);

private:
    struct DeviceRecord {
        UsbDriveInfo   info;
        UsbHsFsDevice* raw = nullptr;
    };

    void refreshLocked();
    void workerMain();
    static std::string providerIdForMountName(const std::string& mountName);

    mutable std::mutex       mutex_;
    std::vector<DeviceRecord> devices_;
    std::atomic<bool>        changed_ {false};
    std::atomic<bool>        running_ {false};
    std::thread              worker_;
    UEvent*                  statusEvent_ = nullptr;
    UEvent*                  exitEvent_ = nullptr;
};

} // namespace xxplore::fs
