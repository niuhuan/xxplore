#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xxplore {

class FtpServer {
public:
    struct Session;

    FtpServer() = default;
    ~FtpServer() { stop(); }

    bool start();
    void stop();

    bool isRunning() const { return running_.load(); }
    std::string url() const;
    std::string status() const;
    std::vector<std::string> logs() const;
    std::size_t sessionCount() const;

private:
    bool bindListenSocket(uint16_t& outPort);
    std::string detectLocalIp() const;
    void workerMain();
    void sessionMain(const std::shared_ptr<Session>& session);
    void reapFinishedSessions();
    void appendLog(const std::string& line);
    void setStatus(const std::string& value);
    void closeListenSocket();

    std::atomic<bool> running_ {false};
    std::atomic<bool> stopRequested_ {false};
    int listenFd_ = -1;
    uint16_t port_ = 0;
    std::thread worker_;

    mutable std::mutex mutex_;
    std::string url_;
    std::string status_ = "Stopped";
    std::deque<std::string> logs_;
    int nextSessionId_ = 1;
    std::vector<std::shared_ptr<Session>> sessions_;
};

} // namespace xxplore
