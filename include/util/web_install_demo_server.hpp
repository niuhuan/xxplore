#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xplore {

class WebInstallDemoServer {
public:
    WebInstallDemoServer() = default;
    ~WebInstallDemoServer() { stop(); }

    bool start();
    void stop();

    bool isRunning() const { return running_; }
    std::string url() const;
    std::string status() const;
    std::vector<std::string> logs() const;

private:
    bool bindListenSocket(uint16_t& outPort);
    std::string detectLocalIp();
    void workerMain();
    void appendLog(const std::string& line);
    void setStatus(const std::string& value);
    void closeClientSocket();
    void closeListenSocket();

    std::string readHttpRequest(int fd);
    void serveIndexPage(int fd);
    void serveNotFound(int fd);
    bool tryUpgradeWebSocket(int fd, const std::string& request);
    void runWebSocketSession(int fd);

    static std::string buildIndexHtml();
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
    int listenFd_ = -1;
    int clientFd_ = -1;
    uint16_t port_ = 0;
    std::thread worker_;

    mutable std::mutex mutex_;
    std::string url_;
    std::string status_ = "Stopped";
    std::deque<std::string> logs_;
};

} // namespace xplore
