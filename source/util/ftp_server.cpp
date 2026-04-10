#include "util/ftp_server.hpp"

#include "fs/fs_api.hpp"

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <memory>
#include <netinet/in.h>
#include <new>
#include <stdexcept>
#include <sstream>
#include <string>
#include <switch.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace xxplore {

namespace {

constexpr uint16_t kPortStart = 5000;
constexpr uint16_t kPortEnd = 5009;
constexpr size_t kMaxLogs = 30;
constexpr int kAcceptTimeoutMs = 15000;
constexpr size_t kTransferBufferSize = 64 * 1024;
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

#ifdef XXPLORE_DEBUG
constexpr const char* kDebugLogPath = "sdmc:/switch/xxplore_ftp_server.log";

void appendDebugFileLine(const char* line) {
    FILE* file = std::fopen(kDebugLogPath, "ab");
    if (!file)
        return;
    std::fwrite(line, 1, std::strlen(line), file);
    std::fwrite("\n", 1, 1, file);
    std::fflush(file);
    std::fclose(file);
}

void debugLog(const char* fmt, ...) {
    char buffer[2048];
    std::va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    std::printf("[ftp-server] ");
    std::printf("%s", buffer);
    std::printf("\n");
    appendDebugFileLine(buffer);
}
#else
void appendDebugFileLine(const char*) {}
void debugLog(const char*, ...) {}
#endif

void closeFd(int& fd) {
    if (fd >= 0) {
        debugLog("close fd=%d", fd);
        ::close(fd);
        fd = -1;
    }
}

bool waitForFd(int fd, bool wantWrite, int timeoutMs) {
    if (fd < 0)
        return false;

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    timeval tv {};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int rc = ::select(fd + 1, wantWrite ? nullptr : &fds, wantWrite ? &fds : nullptr, nullptr, &tv);
    return rc > 0 && FD_ISSET(fd, &fds);
}

bool sendAll(int fd, const void* data, size_t size) {
    if (fd < 0)
        return false;

    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    while (size > 0) {
        ssize_t written = ::send(fd, ptr, size, kSendFlags);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            debugLog("send failed fd=%d errno=%d msg=%s", fd, errno, std::strerror(errno));
            return false;
        }
        if (written == 0)
            return false;
        ptr += written;
        size -= static_cast<size_t>(written);
    }
    return true;
}

std::string trim(const std::string& value) {
    std::size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t'))
        ++start;
    std::size_t end = value.size();
    while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\t'))
        --end;
    return value.substr(start, end - start);
}

std::string toUpperCopy(std::string value) {
    for (char& ch : value)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return value;
}

std::string joinFtpPath(const std::string& base, const std::string& child) {
    if (child.empty())
        return base.empty() ? "/" : base;

    std::vector<std::string> parts;
    std::string path = child[0] == '/' ? child : (base == "/" ? "/" + child : base + "/" + child);
    std::size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/')
            ++i;
        std::size_t start = i;
        while (i < path.size() && path[i] != '/')
            ++i;
        if (i == start)
            continue;
        std::string part = path.substr(start, i - start);
        if (part == ".")
            continue;
        if (part == "..") {
            if (!parts.empty())
                parts.pop_back();
            continue;
        }
        parts.push_back(std::move(part));
    }

    std::string out = "/";
    for (std::size_t index = 0; index < parts.size(); ++index) {
        out += parts[index];
        if (index + 1 < parts.size())
            out += '/';
    }
    return out;
}

std::string ftpToLocal(const std::string& ftpPath) {
    return ftpPath == "/" ? "sdmc:/" : ("sdmc:" + ftpPath);
}

std::string baseNameOfPath(const std::string& path) {
    if (path.empty() || path == "/")
        return {};
    std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return path;
    return path.substr(slash + 1);
}

bool statPathWithTimes(const std::string& localPath, struct stat& st) {
    std::memset(&st, 0, sizeof(st));
    return ::stat(localPath.c_str(), &st) == 0;
}

std::string formatMlsdTime(std::time_t rawTime) {
    std::tm* tm = std::gmtime(&rawTime);
    if (!tm)
        return "19700101000000";
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d%H%M%S", tm);
    return buf;
}

std::string formatListTime(std::time_t rawTime) {
    std::tm* tm = std::localtime(&rawTime);
    if (!tm)
        return "Jan 01 00:00";
    char buf[32];
    std::time_t now = std::time(nullptr);
    std::tm* nowTm = std::localtime(&now);
    if (nowTm && nowTm->tm_year == tm->tm_year)
        std::strftime(buf, sizeof(buf), "%b %d %H:%M", tm);
    else
        std::strftime(buf, sizeof(buf), "%b %d  %Y", tm);
    return buf;
}

std::string buildListLine(const fs::FileEntry& entry, const struct stat& st) {
    std::ostringstream ss;
    ss << (entry.isDirectory ? 'd' : '-')
       << "rw-rw-rw- 1 switch switch "
       << static_cast<unsigned long long>(entry.size)
       << ' ' << formatListTime(st.st_mtime)
       << ' ' << entry.name << "\r\n";
    return ss.str();
}

std::string buildMlsdLine(const fs::FileEntry& entry, const struct stat& st) {
    std::ostringstream ss;
    ss << "type=" << (entry.isDirectory ? "dir" : "file") << ';'
       << "size=" << static_cast<unsigned long long>(entry.size) << ';'
       << "modify=" << formatMlsdTime(st.st_mtime) << ';'
       << ' ' << entry.name << "\r\n";
    return ss.str();
}

} // namespace

struct FtpServer::Session {
    int id = 0;
    int controlFd = -1;
    int passiveListenFd = -1;
    int dataFd = -1;
    sockaddr_in peerAddr {};
    std::atomic<bool> stopRequested {false};
    std::atomic<bool> finished {false};
    std::thread worker;
    std::string cwd = "/";
    std::string renameFrom;
    uint64_t restOffset = 0;
    FtpServer* owner = nullptr;

    void closeSockets() {
        closeFd(dataFd);
        closeFd(passiveListenFd);
        closeFd(controlFd);
    }
};

bool FtpServer::start() {
    stop();

#ifdef XXPLORE_DEBUG
    FILE* reset = std::fopen(kDebugLogPath, "wb");
    if (reset)
        std::fclose(reset);
#endif

    uint16_t port = 0;
    if (!bindListenSocket(port))
        return false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        port_ = port;
        url_ = "ftp://" + detectLocalIp() + ":" + std::to_string(port_) + "/";
        status_ = "Waiting for FTP clients...";
        logs_.clear();
    }

    stopRequested_ = false;
    running_ = true;
    appendLog("FTP server ready.");
    appendLog(url_);
    appendLog("Root maps to sdmc:/");
    worker_ = std::thread([this]() { workerMain(); });
    return true;
}

void FtpServer::stop() {
    stopRequested_ = true;
    closeListenSocket();

    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions = sessions_;
    }
    for (const auto& session : sessions) {
        session->stopRequested = true;
        session->closeSockets();
    }
    for (const auto& session : sessions) {
        if (session->worker.joinable())
            session->worker.join();
    }
    if (worker_.joinable())
        worker_.join();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.clear();
        url_.clear();
        status_ = "Stopped";
        port_ = 0;
    }
    running_ = false;
}

std::string FtpServer::url() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return url_;
}

std::string FtpServer::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

std::vector<std::string> FtpServer::logs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<std::string>(logs_.begin(), logs_.end());
}

std::size_t FtpServer::sessionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

bool FtpServer::bindListenSocket(uint16_t& outPort) {
    for (uint16_t port = kPortStart; port <= kPortEnd; ++port) {
        int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (fd < 0)
            continue;

        int yes = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
            ::listen(fd, 4) == 0) {
            int flags = ::fcntl(fd, F_GETFL, 0);
            if (flags >= 0)
                ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            listenFd_ = fd;
            outPort = port;
            return true;
        }
        ::close(fd);
    }

    appendLog("Failed to bind FTP server.");
    setStatus("Bind failed");
    return false;
}

std::string FtpServer::detectLocalIp() const {
    u32 rawIp = 0;
    if (R_SUCCEEDED(nifmInitialize(NifmServiceType_User))) {
        nifmGetCurrentIpAddress(&rawIp);
        nifmExit();
    }
    in_addr addr {};
    addr.s_addr = rawIp;
    char buf[64] = {0};
    if (::inet_ntop(AF_INET, &addr, buf, sizeof(buf)) == nullptr || buf[0] == '\0')
        std::snprintf(buf, sizeof(buf), "0.0.0.0");
    return buf;
}

void FtpServer::workerMain() {
    try {
        while (!stopRequested_) {
            reapFinishedSessions();
            if (listenFd_ < 0)
                break;
            if (!waitForFd(listenFd_, false, 200))
                continue;

            sockaddr_in peer {};
            socklen_t peerLen = sizeof(peer);
            int fd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&peer), &peerLen);
            if (fd < 0)
                continue;

            int flags = ::fcntl(fd, F_GETFL, 0);
            if (flags >= 0)
                ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

            char ipBuf[64] = {0};
            ::inet_ntop(AF_INET, &peer.sin_addr, ipBuf, sizeof(ipBuf));

            auto session = std::make_shared<Session>();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                session->id = nextSessionId_++;
                session->owner = this;
                session->controlFd = fd;
                session->peerAddr = peer;
                sessions_.push_back(session);
                status_ = "FTP client connected.";
            }

            appendLog(std::string("Client connected: ") + ipBuf);
            debugLog("session id=%d control_fd=%d", session->id, session->controlFd);
            session->worker = std::thread([this, session]() { sessionMain(session); });
        }
    } catch (const std::exception& ex) {
        appendLog(std::string("worker exception: ") + ex.what());
    } catch (...) {
        appendLog("worker exception: unknown");
    }
    reapFinishedSessions();
}

void FtpServer::reapFinishedSessions() {
    std::vector<std::shared_ptr<Session>> finished;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.begin();
        while (it != sessions_.end()) {
            if (!(*it)->finished.load()) {
                ++it;
                continue;
            }
            finished.push_back(*it);
            it = sessions_.erase(it);
        }
        status_ = sessions_.empty() ? "Waiting for FTP clients..." : "FTP client connected.";
    }
    for (const auto& session : finished) {
        if (session->worker.joinable())
            session->worker.join();
    }
}

void FtpServer::appendLog(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    logs_.push_back(line);
    if (logs_.size() > kMaxLogs)
        logs_.pop_front();
    debugLog("%s", line.c_str());
}

void FtpServer::setStatus(const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = value;
}

void FtpServer::closeListenSocket() {
    closeFd(listenFd_);
}

namespace {

bool sessionSendReply(FtpServer::Session& session, int code, const std::string& message) {
    char prefix[8];
    std::snprintf(prefix, sizeof(prefix), "%d ", code);
    std::string line = std::string(prefix) + message + "\r\n";
    debugLog("reply fd=%d code=%d msg=%s", session.controlFd, code, message.c_str());
    return sendAll(session.controlFd, line.data(), line.size());
}

bool sessionSendMultilineStart(FtpServer::Session& session, int code, const std::string& message) {
    char prefix[8];
    std::snprintf(prefix, sizeof(prefix), "%d-", code);
    std::string line = std::string(prefix) + message + "\r\n";
    return sendAll(session.controlFd, line.data(), line.size());
}

bool sessionRecvLine(FtpServer::Session& session, std::string& outLine) {
    outLine.clear();
    char ch = 0;
    while (!session.stopRequested.load()) {
        if (!waitForFd(session.controlFd, false, 200))
            continue;
        ssize_t rc = ::recv(session.controlFd, &ch, 1, 0);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            debugLog("recv command failed fd=%d errno=%d msg=%s",
                     session.controlFd, errno, std::strerror(errno));
            return false;
        }
        if (rc == 0)
            return false;
        if (ch == '\n')
            break;
        if (ch != '\r')
            outLine.push_back(ch);
        if (outLine.size() > 2048)
            return false;
    }
    return !session.stopRequested.load();
}

bool sessionPreparePassive(FtpServer::Session& session, std::string localIp, bool epsv,
                           std::string& errOut, std::string& responseOut) {
    closeFd(session.dataFd);
    closeFd(session.passiveListenFd);

    int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        errOut = "socket failed";
        return false;
    }

    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(fd, 1) != 0) {
        errOut = std::string("bind/listen failed: ") + std::strerror(errno);
        ::close(fd);
        return false;
    }

    sockaddr_in bound {};
    socklen_t boundLen = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &boundLen) != 0) {
        errOut = "getsockname failed";
        ::close(fd);
        return false;
    }

    session.passiveListenFd = fd;
    uint16_t port = ntohs(bound.sin_port);
    debugLog("prepare passive session=%d listen_fd=%d port=%u epsv=%d",
             session.id, session.passiveListenFd, static_cast<unsigned>(port), epsv ? 1 : 0);
    if (epsv) {
        responseOut = "Entering Extended Passive Mode (|||" + std::to_string(port) + "|)";
        return true;
    }

    unsigned a = 127, b = 0, c = 0, d = 1;
    if (std::sscanf(localIp.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        a = 127; b = 0; c = 0; d = 1;
    }
    responseOut = "Entering Passive Mode (" + std::to_string(a) + "," + std::to_string(b) + "," +
                  std::to_string(c) + "," + std::to_string(d) + "," +
                  std::to_string((port >> 8) & 0xff) + "," + std::to_string(port & 0xff) + ")";
    return true;
}

bool sessionAcceptData(FtpServer::Session& session, std::string& errOut) {
    if (session.passiveListenFd < 0) {
        errOut = "use PASV or EPSV first";
        return false;
    }
    if (!waitForFd(session.passiveListenFd, false, kAcceptTimeoutMs)) {
        errOut = "data connection timed out";
        closeFd(session.passiveListenFd);
        return false;
    }

    sockaddr_in peer {};
    socklen_t peerLen = sizeof(peer);
    int fd = ::accept(session.passiveListenFd, reinterpret_cast<sockaddr*>(&peer), &peerLen);
    closeFd(session.passiveListenFd);
    if (fd < 0) {
        errOut = std::string("accept failed: ") + std::strerror(errno);
        return false;
    }
    session.dataFd = fd;
    debugLog("accept data session=%d data_fd=%d", session.id, session.dataFd);
    return true;
}

void sessionCloseData(FtpServer::Session& session) {
    debugLog("close data session=%d passive_fd=%d data_fd=%d rest=%llu",
             session.id, session.passiveListenFd, session.dataFd,
             static_cast<unsigned long long>(session.restOffset));
    closeFd(session.dataFd);
    closeFd(session.passiveListenFd);
    session.restOffset = 0;
}

void sessionAbortPendingTransfer(FtpServer::Session& session) {
    sessionCloseData(session);
}

bool validateTargetName(const std::string& ftpPath, std::string& errOut) {
    std::string base = baseNameOfPath(ftpPath);
    debugLog("validate target ftp_path=%s base=%s", ftpPath.c_str(), base.c_str());
    if (base.empty())
        return true;
    if (fs::isValidEnglishFileName(base))
        return true;
    errOut = "non-ASCII names are not supported";
    return false;
}

bool doListTransfer(FtpServer::Session& session, const std::string& ftpPath, bool mlsd,
                    std::string& errOut) {
    std::string localPath = ftpToLocal(ftpPath);
    std::vector<fs::FileEntry> entries = fs::listDir(localPath);
    if (!fs::pathExists(localPath)) {
        errOut = "path not found";
        return false;
    }
    if (!sessionSendReply(session, 150, "Opening data connection"))
        return false;
    if (!sessionAcceptData(session, errOut)) {
        sessionSendReply(session, 425, errOut);
        return false;
    }

    bool ok = true;
    for (const auto& entry : entries) {
        struct stat st {};
        if (!statPathWithTimes(fs::joinPath(localPath, entry.name), st))
            std::memset(&st, 0, sizeof(st));
        std::string line = mlsd ? buildMlsdLine(entry, st) : buildListLine(entry, st);
        if (!sendAll(session.dataFd, line.data(), line.size())) {
            errOut = "data send failed";
            ok = false;
            break;
        }
    }
    sessionCloseData(session);
    if (!ok)
        return sessionSendReply(session, 426, errOut);
    return sessionSendReply(session, 226, "Transfer complete");
}

bool doFileDownload(FtpServer::Session& session, const std::string& ftpPath, std::string& errOut) {
    debugLog("download begin session=%d ftp_path=%s", session.id, ftpPath.c_str());
    std::string localPath = ftpToLocal(ftpPath);
    FILE* file = std::fopen(localPath.c_str(), "rb");
    if (!file) {
        errOut = std::strerror(errno);
        return false;
    }

    if (session.restOffset > 0 &&
        ::fseeko(file, static_cast<off_t>(session.restOffset), SEEK_SET) != 0) {
        errOut = std::strerror(errno);
        std::fclose(file);
        return false;
    }

    if (!sessionSendReply(session, 150, "Opening binary mode data connection")) {
        std::fclose(file);
        return false;
    }
    if (!sessionAcceptData(session, errOut)) {
        std::fclose(file);
        sessionSendReply(session, 425, errOut);
        return false;
    }

    std::unique_ptr<char[]> buffer(new (std::nothrow) char[kTransferBufferSize]);
    if (!buffer) {
        std::fclose(file);
        sessionCloseData(session);
        errOut = "download buffer allocation failed";
        return false;
    }
    debugLog("download buffer ready session=%d size=%zu", session.id, kTransferBufferSize);

    bool ok = true;
    while (!session.stopRequested.load()) {
        size_t readBytes = std::fread(buffer.get(), 1, kTransferBufferSize, file);
        if (readBytes > 0 && !sendAll(session.dataFd, buffer.get(), readBytes)) {
            errOut = "data send failed";
            ok = false;
            break;
        }
        if (readBytes < kTransferBufferSize) {
            if (std::ferror(file)) {
                errOut = std::strerror(errno);
                ok = false;
            }
            break;
        }
    }

    std::fclose(file);
    sessionCloseData(session);
    if (!ok)
        return sessionSendReply(session, 426, errOut);
    return sessionSendReply(session, 226, "Transfer complete");
}

bool doFileUpload(FtpServer::Session& session, const std::string& ftpPath, std::string& errOut) {
    debugLog("upload begin session=%d ftp_path=%s", session.id, ftpPath.c_str());
    if (!validateTargetName(ftpPath, errOut))
        return false;

    std::string localPath = ftpToLocal(ftpPath);
    std::string parent = fs::parentPath(localPath);
    debugLog("upload path resolved local=%s parent=%s", localPath.c_str(), parent.c_str());
    if (!parent.empty() && !fs::pathExists(parent)) {
        errOut = "parent directory not found";
        return false;
    }

    const bool resume = session.restOffset > 0 && fs::pathExists(localPath);
    FILE* file = std::fopen(localPath.c_str(), resume ? "r+b" : "wb");
    if (!file) {
        errOut = std::strerror(errno);
        return false;
    }
    if (session.restOffset > 0 &&
        ::fseeko(file, static_cast<off_t>(session.restOffset), SEEK_SET) != 0) {
        errOut = std::strerror(errno);
        std::fclose(file);
        return false;
    }

    if (!sessionSendReply(session, 150, "Opening binary mode data connection")) {
        std::fclose(file);
        return false;
    }
    if (!sessionAcceptData(session, errOut)) {
        std::fclose(file);
        sessionSendReply(session, 425, errOut);
        return false;
    }

    std::unique_ptr<char[]> buffer(new (std::nothrow) char[kTransferBufferSize]);
    if (!buffer) {
        std::fclose(file);
        sessionCloseData(session);
        errOut = "upload buffer allocation failed";
        return false;
    }
    debugLog("upload buffer ready session=%d size=%zu", session.id, kTransferBufferSize);

    bool ok = true;
    while (!session.stopRequested.load()) {
        ssize_t readBytes = ::recv(session.dataFd, buffer.get(), kTransferBufferSize, 0);
        if (readBytes < 0) {
            if (errno == EINTR)
                continue;
            errOut = std::strerror(errno);
            ok = false;
            break;
        }
        if (readBytes == 0)
            break;
        if (std::fwrite(buffer.get(), 1, static_cast<size_t>(readBytes), file) !=
            static_cast<size_t>(readBytes)) {
            errOut = std::strerror(errno);
            ok = false;
            break;
        }
    }

    std::fclose(file);
    sessionCloseData(session);
    if (!ok)
        return sessionSendReply(session, 426, errOut);
    return sessionSendReply(session, 226, "Transfer complete");
}

bool doMlst(FtpServer::Session& session, const std::string& ftpPath, std::string& errOut) {
    std::string localPath = ftpToLocal(ftpPath);
    struct stat st {};
    if (!statPathWithTimes(localPath, st)) {
        errOut = "path not found";
        return false;
    }
    fs::FileEntry entry;
    entry.name = baseNameOfPath(ftpPath);
    entry.isDirectory = S_ISDIR(st.st_mode);
    entry.size = static_cast<uint64_t>(st.st_size);

    std::string line = " " + buildMlsdLine(entry, st);
    if (!sessionSendMultilineStart(session, 250, "Listing"))
        return false;
    if (!sendAll(session.controlFd, line.data(), line.size()))
        return false;
    return sessionSendReply(session, 250, "End");
}

} // namespace

void FtpServer::sessionMain(const std::shared_ptr<Session>& session) {
    char ipBuf[64] = {0};
    ::inet_ntop(AF_INET, &session->peerAddr.sin_addr, ipBuf, sizeof(ipBuf));
    const std::string peerLabel(ipBuf);
    try {
        auto logCommand = [&](const std::string& line) {
            appendLog("[" + peerLabel + "] " + line);
        };

        auto failReply = [&](const char* cmd, const std::string& err, int code = 550,
                             bool abortPendingTransfer = false) -> bool {
            appendLog("[" + peerLabel + "] " + std::string(cmd) + " failed: " + err);
            debugLog("failReply session=%d cmd=%s code=%d abort=%d control_fd=%d passive_fd=%d data_fd=%d err=%s",
                     session->id, cmd, code, abortPendingTransfer ? 1 : 0,
                     session->controlFd, session->passiveListenFd, session->dataFd, err.c_str());
            if (abortPendingTransfer)
                sessionAbortPendingTransfer(*session);
            return sessionSendReply(*session, code, err);
        };

        if (!sessionSendReply(*session, 220, "FTP server ready")) {
            session->closeSockets();
            session->finished = true;
            return;
        }

        bool running = true;
        while (running && !stopRequested_.load() && !session->stopRequested.load()) {
            std::string line;
            if (!sessionRecvLine(*session, line))
                break;
            if (line.empty())
                continue;

            std::string arg;
            std::size_t space = line.find(' ');
            if (space != std::string::npos) {
                arg = trim(line.substr(space + 1));
                line.resize(space);
            }
            std::string cmd = toUpperCopy(line);
            logCommand(cmd + (arg.empty() ? "" : " " + arg));

            if (cmd == "USER") {
                if (!sessionSendReply(*session, 331, "Password required"))
                    break;
            } else if (cmd == "PASS") {
                if (!sessionSendReply(*session, 230, "Login successful"))
                    break;
            } else if (cmd == "SYST") {
                if (!sessionSendReply(*session, 215, "UNIX Type: L8"))
                    break;
            } else if (cmd == "FEAT") {
                if (!sessionSendMultilineStart(*session, 211, "Extensions supported"))
                    break;
                const char* feat =
                    " UTF8\r\n"
                    " EPSV\r\n"
                    " PASV\r\n"
                    " SIZE\r\n"
                    " MDTM\r\n"
                    " MLSD\r\n"
                    " MLST type*;size*;modify*;\r\n"
                    " REST STREAM\r\n";
                if (!sendAll(session->controlFd, feat, std::strlen(feat)))
                    break;
                if (!sessionSendReply(*session, 211, "END"))
                    break;
            } else if (cmd == "OPTS") {
                if (!sessionSendReply(*session, 200, "OK"))
                    break;
            } else if (cmd == "PWD" || cmd == "XPWD") {
                if (!sessionSendReply(*session, 257, "\"" + session->cwd + "\""))
                    break;
            } else if (cmd == "CWD") {
                std::string ftpPath = joinFtpPath(session->cwd, arg.empty() ? "/" : arg);
                std::string localPath = ftpToLocal(ftpPath);
                if (!fs::isDirectoryPath(localPath)) {
                    if (!failReply("CWD", "directory not found"))
                        break;
                } else if (!sessionSendReply(*session, 250, "Directory changed")) {
                    break;
                } else {
                    session->cwd = ftpPath;
                }
            } else if (cmd == "CDUP") {
                session->cwd = joinFtpPath(session->cwd, "..");
                if (!sessionSendReply(*session, 200, "OK"))
                    break;
            } else if (cmd == "TYPE") {
                if (!sessionSendReply(*session, 200, "Type set"))
                    break;
            } else if (cmd == "MODE" || cmd == "STRU") {
                if (!sessionSendReply(*session, 200, "OK"))
                    break;
            } else if (cmd == "NOOP") {
                if (!sessionSendReply(*session, 200, "OK"))
                    break;
            } else if (cmd == "ALLO") {
                if (!sessionSendReply(*session, 202, "Ignored"))
                    break;
            } else if (cmd == "EPSV" || cmd == "PASV") {
                std::string err;
                std::string reply;
                if (!sessionPreparePassive(*session, detectLocalIp(), cmd == "EPSV", err, reply)) {
                    if (!failReply(cmd.c_str(), err, 425, true))
                        break;
                } else if (!sessionSendReply(*session, cmd == "EPSV" ? 229 : 227, reply)) {
                    break;
                }
            } else if (cmd == "LIST" || cmd == "NLST") {
                std::string ftpPath = joinFtpPath(session->cwd, arg.empty() ? "." : arg);
                std::string err;
                if (!doListTransfer(*session, ftpPath, false, err))
                    appendLog("[" + peerLabel + "] LIST failed: " + err);
            } else if (cmd == "MLSD") {
                std::string ftpPath = joinFtpPath(session->cwd, arg.empty() ? "." : arg);
                std::string err;
                if (!doListTransfer(*session, ftpPath, true, err))
                    appendLog("[" + peerLabel + "] MLSD failed: " + err);
            } else if (cmd == "MLST") {
                std::string ftpPath = joinFtpPath(session->cwd, arg.empty() ? "." : arg);
                std::string err;
                if (!doMlst(*session, ftpPath, err)) {
                    if (!failReply("MLST", err))
                        break;
                }
            } else if (cmd == "SIZE") {
                std::string ftpPath = joinFtpPath(session->cwd, arg);
                struct stat st {};
                if (!statPathWithTimes(ftpToLocal(ftpPath), st) || S_ISDIR(st.st_mode)) {
                    if (!failReply("SIZE", "file not found"))
                        break;
                } else if (!sessionSendReply(
                               *session, 213,
                               std::to_string(static_cast<unsigned long long>(st.st_size)))) {
                    break;
                }
            } else if (cmd == "MDTM") {
                std::string ftpPath = joinFtpPath(session->cwd, arg);
                struct stat st {};
                if (!statPathWithTimes(ftpToLocal(ftpPath), st)) {
                    if (!failReply("MDTM", "path not found"))
                        break;
                } else if (!sessionSendReply(*session, 213, formatMlsdTime(st.st_mtime))) {
                    break;
                }
            } else if (cmd == "REST") {
                char* end = nullptr;
                unsigned long long value = std::strtoull(arg.c_str(), &end, 10);
                if (!end || *end != '\0') {
                    if (!failReply("REST", "invalid offset"))
                        break;
                } else {
                    session->restOffset = static_cast<uint64_t>(value);
                    if (!sessionSendReply(*session, 350, "Restart position accepted"))
                        break;
                }
            } else if (cmd == "RETR") {
                std::string ftpPath = joinFtpPath(session->cwd, arg);
                std::string err;
                if (!doFileDownload(*session, ftpPath, err)) {
                    if (!failReply("RETR", err, 550, true))
                        break;
                }
            } else if (cmd == "STOR") {
                std::string ftpPath = joinFtpPath(session->cwd, arg);
                debugLog("STOR session=%d arg=%s cwd=%s ftp_path=%s control_fd=%d passive_fd=%d data_fd=%d",
                         session->id, arg.c_str(), session->cwd.c_str(), ftpPath.c_str(),
                         session->controlFd, session->passiveListenFd, session->dataFd);
                std::string err;
                if (!doFileUpload(*session, ftpPath, err)) {
                    int code = (err == "non-ASCII names are not supported") ? 553 : 550;
                    if (!failReply("STOR", err, code, true))
                        break;
                }
            } else if (cmd == "DELE") {
                std::string ftpPath = joinFtpPath(session->cwd, arg);
                std::string localPath = ftpToLocal(ftpPath);
                std::string err;
                if (fs::isDirectoryPath(localPath)) {
                    if (!failReply("DELE", "path is a directory"))
                        break;
                } else if (!fs::removeAll(localPath, err)) {
                    if (!failReply("DELE", err))
                        break;
                } else if (!sessionSendReply(*session, 250, "Deleted")) {
                    break;
                }
            } else if (cmd == "RMD" || cmd == "XRMD") {
                std::string ftpPath = joinFtpPath(session->cwd, arg);
                std::string err;
                if (!fs::removeAll(ftpToLocal(ftpPath), err)) {
                    if (!failReply(cmd.c_str(), err))
                        break;
                } else if (!sessionSendReply(*session, 250, "Removed")) {
                    break;
                }
            } else if (cmd == "MKD" || cmd == "XMKD") {
                std::string ftpPath = joinFtpPath(session->cwd, arg);
                std::string err;
                if (!validateTargetName(ftpPath, err)) {
                    if (!failReply(cmd.c_str(), err, 553))
                        break;
                } else if (!fs::createDirectory(ftpToLocal(ftpPath), err)) {
                    if (!failReply(cmd.c_str(), err))
                        break;
                } else if (!sessionSendReply(*session, 257, "\"" + ftpPath + "\"")) {
                    break;
                }
            } else if (cmd == "RNFR") {
                std::string ftpPath = joinFtpPath(session->cwd, arg);
                if (!fs::pathExists(ftpToLocal(ftpPath))) {
                    if (!failReply("RNFR", "path not found"))
                        break;
                } else {
                    session->renameFrom = ftpPath;
                    if (!sessionSendReply(*session, 350, "Ready for RNTO"))
                        break;
                }
            } else if (cmd == "RNTO") {
                if (session->renameFrom.empty()) {
                    if (!failReply("RNTO", "RNFR required first"))
                        break;
                } else {
                    std::string ftpPath = joinFtpPath(session->cwd, arg);
                    std::string err;
                    if (!validateTargetName(ftpPath, err)) {
                        if (!failReply("RNTO", err, 553))
                            break;
                    } else if (!fs::renamePath(ftpToLocal(session->renameFrom),
                                                ftpToLocal(ftpPath), err)) {
                        if (!failReply("RNTO", err))
                            break;
                    } else if (!sessionSendReply(*session, 250, "Rename successful")) {
                        break;
                    }
                    session->renameFrom.clear();
                }
            } else if (cmd == "ABOR") {
                sessionCloseData(*session);
                if (!sessionSendReply(*session, 225, "No transfer in progress"))
                    break;
            } else if (cmd == "PORT" || cmd == "EPRT") {
                if (!sessionSendReply(*session, 502, "Active mode is not supported"))
                    break;
            } else if (cmd == "AUTH" || cmd == "PBSZ" || cmd == "PROT") {
                if (!sessionSendReply(*session, 502, "TLS is not supported"))
                    break;
            } else if (cmd == "QUIT") {
                sessionSendReply(*session, 221, "Bye");
                running = false;
            } else {
                if (!sessionSendReply(*session, 502, "Command not implemented"))
                    break;
            }
        }
    } catch (const std::exception& ex) {
        appendLog("[" + peerLabel + "] exception: " + ex.what());
    } catch (...) {
        appendLog("[" + peerLabel + "] exception: unknown");
    }

    sessionAbortPendingTransfer(*session);
    session->closeSockets();
    appendLog("Client disconnected: " + peerLabel);
    session->finished = true;
}

} // namespace xxplore
