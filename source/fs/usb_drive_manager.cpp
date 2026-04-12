#include "fs/usb_drive_manager.hpp"
#include <switch.h>
#include <usbhsfs.h>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace xxplore::fs {

namespace {

constexpr const char* USB_DEBUG_LOG_PATH = "sdmc:/libusbhsfs.log";

std::string formatUsbResult(const char* what, Result rc) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s failed: 0x%X", what, static_cast<unsigned>(rc));
    return std::string(buf);
}

void cleanupUsbDebugLogIfNeeded() {
    struct stat st {};
    if (::stat(USB_DEBUG_LOG_PATH, &st) != 0)
        return;
    if (st.st_size <= 0)
        return;
    ::unlink(USB_DEBUG_LOG_PATH);
}

} // namespace

UsbDriveManager::~UsbDriveManager() {
    shutdown();
}

std::string UsbDriveManager::providerIdForMountName(const std::string& mountName) {
    std::string id = "usb-";
    for (char c : mountName) {
        if (c == ':')
            continue;
        id.push_back(c);
    }
    return id;
}

bool UsbDriveManager::init(std::string& errOut) {
    if (running_)
        return true;

    cleanupUsbDebugLogIfNeeded();

    Result rc = usbHsFsInitialize(0);
    if (R_FAILED(rc)) {
        errOut = formatUsbResult("usbHsFsInitialize", rc);
        return false;
    }

    statusEvent_ = usbHsFsGetStatusChangeUserEvent();
    if (!statusEvent_) {
        errOut = "usbHsFsGetStatusChangeUserEvent returned null";
        usbHsFsExit();
        return false;
    }

    exitEvent_ = new UEvent{};
    ueventCreate(exitEvent_, true);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        refreshLocked();
    }
    changed_ = true;
    running_ = true;
    worker_ = std::thread(&UsbDriveManager::workerMain, this);
    return true;
}

void UsbDriveManager::shutdown() {
    if (!statusEvent_ && !running_)
        return;

    running_ = false;
    if (exitEvent_)
        ueventSignal(exitEvent_);
    if (worker_.joinable())
        worker_.join();
    if (exitEvent_) {
        delete exitEvent_;
        exitEvent_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& device : devices_)
            delete device.raw;
        devices_.clear();
    }
    statusEvent_ = nullptr;
    changed_ = false;
    usbHsFsExit();
}

std::vector<UsbDriveInfo> UsbDriveManager::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<UsbDriveInfo> out;
    out.reserve(devices_.size());
    for (const auto& device : devices_)
        out.push_back(device.info);
    return out;
}

bool UsbDriveManager::consumeChanged(std::vector<UsbDriveInfo>& out) {
    if (!changed_.exchange(false))
        return false;
    out = snapshot();
    return true;
}

std::vector<UsbDriveInfo> UsbDriveManager::refreshNow() {
    std::lock_guard<std::mutex> lock(mutex_);
    refreshLocked();
    std::vector<UsbDriveInfo> out;
    out.reserve(devices_.size());
    for (const auto& device : devices_)
        out.push_back(device.info);
    changed_ = false;
    return out;
}

bool UsbDriveManager::unmountByProviderId(const std::string& providerId, std::string& errOut) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& device : devices_) {
        if (device.info.providerId != providerId)
            continue;
        if (usbHsFsUnmountDevice(device.raw, true))
            return true;
        errOut = "usbHsFsUnmountDevice failed";
        return false;
    }
    errOut = "USB drive not found";
    return false;
}

void UsbDriveManager::refreshLocked() {
    for (auto& device : devices_)
        delete device.raw;
    devices_.clear();

    u32 count = usbHsFsGetMountedDeviceCount();
    if (!count)
        return;

    std::vector<UsbHsFsDevice> listed(count);
    u32 written = usbHsFsListMountedDevices(listed.data(), count);
    listed.resize(written);
    for (auto& device : listed) {
        DeviceRecord rec;
        rec.info.mountName = device.name;
        rec.info.providerId = providerIdForMountName(device.name);
        rec.info.readOnly = device.write_protect;
        rec.raw = new UsbHsFsDevice{};
        *rec.raw = device;
        devices_.push_back(std::move(rec));
    }
}

void UsbDriveManager::workerMain() {
    if (!statusEvent_ || !exitEvent_)
        return;

    Waiter statusWaiter = waiterForUEvent(statusEvent_);
    Waiter exitWaiter = waiterForUEvent(exitEvent_);

    while (running_) {
        s32 idx = -1;
        Result rc = waitMulti(&idx, UINT64_MAX, statusWaiter, exitWaiter);
        if (R_FAILED(rc))
            continue;
        if (idx == 1)
            break;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            refreshLocked();
        }
        changed_ = true;
    }
}

} // namespace xxplore::fs
