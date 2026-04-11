#pragma once
#include "util/byte_rate_meter.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace xxplore {

enum class WebInstallTarget { Nand, SdCard };
struct InstallSequentialReader;

class WebSocketInstallerServer {
public:
    using TextMap = std::unordered_map<std::string, std::string>;

    WebSocketInstallerServer() = default;
    ~WebSocketInstallerServer() { stop(); }

    bool start(WebInstallTarget target);
    void stop();
    void setTextMap(TextMap textMap);

    bool isRunning() const { return running_; }
    bool isInstalling() const { return installing_; }
    std::string url() const;
    std::string status() const;
    std::vector<std::string> logs() const;
    float currentProgress() const;
    float totalProgress() const;
    uint64_t totalBytes() const;
    size_t itemCount() const;
    std::string currentItem() const;
    WebInstallTarget target() const;
    uint64_t speedBytesPerSec() const;
    bool hasSpeedSample() const;
    uint64_t transferredBytes() const;
    bool speedFinished() const;
    void requestAbortInstall();
    bool abortRequested() const { return abortRequested_.load(); }

private:
    struct RemoteFileEntry {
        std::string id;
        std::string name;
        uint64_t size = 0;
        std::string virtualPath;
    };

    bool bindListenSocket(uint16_t& outPort);
    std::string detectLocalIp();
    void workerMain();
    void installWorkerMain(std::vector<RemoteFileEntry> items);
    void appendLog(const std::string& line);
    void setStatus(const std::string& value);
    void setProgress(float current, float total, const std::string& currentItem);
    void closeClientSocket();
    void closeListenSocket();

    std::string readHttpRequest(int fd);
    void serveIndexPage(int fd);
    void serveNotFound(int fd);
    bool tryUpgradeWebSocket(int fd, const std::string& request);
    void runWebSocketSession(int fd);
    bool handleClientTextMessage(const std::string& payload);
    bool handleClientBinaryMessage(const std::string& payload);
    bool requestRemoteRead(const std::string& fileId, uint64_t offset, size_t size,
                           void* outBuffer, std::string& errOut);
    std::unique_ptr<InstallSequentialReader>
    openRemoteSequentialRead(const std::string& fileId, uint64_t offset, uint64_t expectedSize,
                             std::string& errOut);
    bool openRemoteStream(const std::string& fileId, uint64_t offset, uint64_t expectedSize,
                          uint32_t& streamIdOut, std::string& errOut);
    bool readRemoteStream(uint32_t streamId, void* outBuffer, size_t size, std::string& errOut);
    void closeRemoteStream(uint32_t streamId);
    bool sendJsonEvent(const std::string& type, const std::string& message);
    bool sendProgressEvent(float current, float total, const std::string& currentItem);
    bool sendInstallResult(bool ok, const std::string& message);
    void abortPendingRead(const std::string& reason);

    std::string buildIndexHtml() const;
    std::string text(const char* key) const;
    static std::string websocketAcceptKey(const std::string& clientKey);
    static std::string trim(const std::string& value);
    static std::string headerValue(const std::string& request, const char* name);
    static bool recvExact(int fd, void* buf, size_t size, int timeoutMs,
                          const std::atomic<bool>& stopRequested);
    static bool sendAll(int fd, const void* data, size_t size);
    static bool sendWsFrame(int fd, uint8_t opcode, const void* data, size_t size);
    static bool sendWsText(int fd, const std::string& text);
    static bool recvWsFrame(int fd, uint8_t& opcode, std::string& payload,
                            const std::atomic<bool>& stopRequested);

    std::atomic<bool> running_ {false};
    std::atomic<bool> stopRequested_ {false};
    std::atomic<bool> installing_ {false};
    std::atomic<bool> abortRequested_ {false};
    int listenFd_ = -1;
    int clientFd_ = -1;
    uint16_t port_ = 0;
    WebInstallTarget target_ = WebInstallTarget::SdCard;
    std::thread worker_;
    std::thread installWorker_;
    std::thread sessionWorker_;

    mutable std::mutex mutex_;
    std::string url_;
    std::string status_ = "Stopped";
    std::deque<std::string> logs_;
    TextMap textMap_;
    float currentProgress_ = 0.0f;
    float totalProgress_ = 0.0f;
    uint64_t totalBytes_ = 0;
    size_t itemCount_ = 0;
    std::string currentItem_;
    util::ByteRateMeter speedMeter_;

    std::mutex sendMutex_;
    std::mutex sessionMutex_;
    std::condition_variable sessionCv_;
    bool sessionActive_ = false;
    bool pendingReadReady_ = false;
    uint32_t pendingReadReqId_ = 0;
    std::vector<uint8_t> pendingReadData_;
    std::string pendingReadError_;
    std::string sessionAbortReason_;
    uint32_t nextReqId_ = 1;
    bool streamActive_ = false;
    uint32_t activeStreamId_ = 0;
    uint64_t activeStreamRemaining_ = 0;
    std::deque<std::vector<uint8_t>> streamQueue_;
    std::size_t streamQueueFrontOffset_ = 0;
    std::size_t streamQueueBytes_ = 0;
    bool streamFinished_ = false;
    std::string streamError_;
};

} // namespace xxplore
