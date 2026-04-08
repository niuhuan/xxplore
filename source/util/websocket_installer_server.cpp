#include "util/websocket_installer_server.hpp"

#include "install/backend.hpp"
#include "ui/installer_screen.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <json-c/json.h>
#include <netinet/in.h>
#include <switch.h>
#include <switch/crypto/sha1.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace xplore {

namespace {

constexpr uint16_t kPortStart = 18080;
constexpr uint16_t kPortEnd = 18089;
constexpr size_t kMaxLogs = 30;
constexpr char kWsGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr char kBase64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr char kReadMagic[4] = {'X', 'P', 'R', 'D'};

std::string base64Encode(const uint8_t* data, size_t size) {
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    for (size_t i = 0; i < size; i += 3) {
        uint32_t value = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < size)
            value |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < size)
            value |= static_cast<uint32_t>(data[i + 2]);

        out.push_back(kBase64Table[(value >> 18) & 0x3f]);
        out.push_back(kBase64Table[(value >> 12) & 0x3f]);
        out.push_back(i + 1 < size ? kBase64Table[(value >> 6) & 0x3f] : '=');
        out.push_back(i + 2 < size ? kBase64Table[value & 0x3f] : '=');
    }
    return out;
}

void closeFd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

std::string toLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string fileExtLower(const std::string& name) {
    size_t dot = name.rfind('.');
    if (dot == std::string::npos)
        return "";
    return toLowerCopy(name.substr(dot));
}

bool waitForSocket(int fd, bool wantWrite, int timeoutMs) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    timeval tv {};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int rc = ::select(fd + 1, wantWrite ? nullptr : &fds, wantWrite ? &fds : nullptr, nullptr, &tv);
    return rc > 0 && FD_ISSET(fd, &fds);
}

bool isSupportedInstallExt(const std::string& ext) {
    return ext == ".nsp" || ext == ".nsz" || ext == ".xci" || ext == ".xcz";
}

bool isAsciiId(const std::string& value) {
    if (value.empty())
        return false;
    for (unsigned char c : value) {
        if (!(std::isdigit(c) || (c >= 'A' && c <= 'Z') || c == '-'))
            return false;
    }
    return true;
}

std::string jsonString(json_object* object) {
    return json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN);
}

void jsonAddString(json_object* object, const char* key, const std::string& value) {
    json_object_object_add(object, key, json_object_new_string(value.c_str()));
}

bool parseUint32LE(const uint8_t* ptr, uint32_t& out) {
    out = static_cast<uint32_t>(ptr[0]) |
          (static_cast<uint32_t>(ptr[1]) << 8) |
          (static_cast<uint32_t>(ptr[2]) << 16) |
          (static_cast<uint32_t>(ptr[3]) << 24);
    return true;
}

void replaceAll(std::string& text, const std::string& from, const std::string& to) {
    if (from.empty())
        return;
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string htmlEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

} // namespace

void WebSocketInstallerServer::setTextMap(TextMap textMap) {
    std::lock_guard<std::mutex> lock(mutex_);
    textMap_ = std::move(textMap);
}

std::string WebSocketInstallerServer::text(const char* key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textMap_.find(key);
    if (it != textMap_.end())
        return it->second;
    return key;
}

bool WebSocketInstallerServer::start(WebInstallTarget target) {
    stop();

    uint16_t port = 0;
    if (!bindListenSocket(port))
        return false;

    target_ = target;
    port_ = port;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url_ = "http://" + detectLocalIp() + ":" + std::to_string(port_);
        status_ = textMap_.count("status_waiting_browser")
                      ? textMap_.at("status_waiting_browser")
                      : "Waiting for browser...";
        currentProgress_ = 0.0f;
        totalProgress_ = 0.0f;
        totalBytes_ = 0;
        itemCount_ = 0;
        currentItem_.clear();
    }

    stopRequested_ = false;
    running_ = true;
    installing_ = false;
    appendLog("HTTP server ready.");
    appendLog(url_);
    worker_ = std::thread([this]() { workerMain(); });
    return true;
}

void WebSocketInstallerServer::stop() {
    stopRequested_ = true;
    abortPendingRead(text("error_session_closed"));
    closeClientSocket();
    closeListenSocket();

    if (installWorker_.joinable())
        installWorker_.join();
    if (sessionWorker_.joinable())
        sessionWorker_.join();
    if (worker_.joinable())
        worker_.join();

    running_ = false;
    installing_ = false;
    port_ = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "Stopped";
        currentProgress_ = 0.0f;
        totalProgress_ = 0.0f;
        currentItem_.clear();
        itemCount_ = 0;
        totalBytes_ = 0;
    }
}

std::string WebSocketInstallerServer::url() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return url_;
}

std::string WebSocketInstallerServer::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

std::vector<std::string> WebSocketInstallerServer::logs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<std::string>(logs_.begin(), logs_.end());
}

float WebSocketInstallerServer::currentProgress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentProgress_;
}

float WebSocketInstallerServer::totalProgress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return totalProgress_;
}

uint64_t WebSocketInstallerServer::totalBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return totalBytes_;
}

size_t WebSocketInstallerServer::itemCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return itemCount_;
}

std::string WebSocketInstallerServer::currentItem() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentItem_;
}

WebInstallTarget WebSocketInstallerServer::target() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return target_;
}

bool WebSocketInstallerServer::bindListenSocket(uint16_t& outPort) {
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

    appendLog("Failed to bind HTTP server.");
    setStatus("Bind failed");
    return false;
}

std::string WebSocketInstallerServer::detectLocalIp() {
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

void WebSocketInstallerServer::workerMain() {
    while (!stopRequested_) {
        if (listenFd_ < 0)
            break;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listenFd_, &rfds);
        timeval tv {};
        tv.tv_sec = 0;
        tv.tv_usec = 200000;

        int ready = ::select(listenFd_ + 1, &rfds, nullptr, nullptr, &tv);
        if (ready <= 0)
            continue;
        if (!FD_ISSET(listenFd_, &rfds))
            continue;

        sockaddr_in clientAddr {};
        socklen_t clientLen = sizeof(clientAddr);
        int fd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (fd < 0)
            continue;

        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0)
            ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

        char ipBuf[64] = {0};
        ::inet_ntop(AF_INET, &clientAddr.sin_addr, ipBuf, sizeof(ipBuf));
        appendLog(std::string("Accepted TCP client: ") + ipBuf);

        std::string request = readHttpRequest(fd);
        if (request.empty()) {
            appendLog("Empty or timed-out HTTP request.");
            ::close(fd);
            continue;
        }

        std::string requestLower = toLowerCopy(request);
        size_t lineEnd = request.find("\r\n");
        std::string requestLine = lineEnd == std::string::npos ? request : request.substr(0, lineEnd);
        appendLog("HTTP " + requestLine);

        if (requestLower.find("upgrade: websocket") != std::string::npos &&
            requestLower.find("get /ws ") != std::string::npos) {
            bool busy = false;
            bool abortInstall = false;
            {
                std::lock_guard<std::mutex> lock(sessionMutex_);
                busy = sessionActive_;
                abortInstall = sessionActive_ && installing_.load();
            }
            if (busy) {
                if (abortInstall) {
                    const std::string reason = text("error_conflict_abort");
                    appendLog(reason);
                    setStatus(text("status_install_aborted"));
                    {
                        std::lock_guard<std::mutex> lock(sessionMutex_);
                        sessionAbortReason_ = reason;
                    }
                    sendJsonEvent("error", reason);
                    closeClientSocket();
                    static const char kAbortResp[] =
                        "HTTP/1.1 409 Conflict\r\n"
                        "Content-Type: text/plain; charset=utf-8\r\n"
                        "Connection: close\r\n"
                        "\r\n"
                        "Another browser session was already installing. "
                        "The current install was aborted for safety.\r\n";
                    sendAll(fd, kAbortResp, sizeof(kAbortResp) - 1);
                } else {
                    static const char kBusyResp[] =
                        "HTTP/1.1 409 Conflict\r\n"
                        "Content-Type: text/plain; charset=utf-8\r\n"
                        "Content-Length: 19\r\n"
                        "Connection: close\r\n"
                        "\r\n"
                        "WebSocket busy.\r\n";
                    sendAll(fd, kBusyResp, sizeof(kBusyResp) - 1);
                }
                ::close(fd);
                continue;
            }

            if (!tryUpgradeWebSocket(fd, request)) {
                ::close(fd);
                continue;
            }

            if (sessionWorker_.joinable())
                sessionWorker_.join();

            {
                std::lock_guard<std::mutex> lock(sessionMutex_);
                clientFd_ = fd;
                sessionActive_ = true;
                pendingReadReady_ = false;
                pendingReadReqId_ = 0;
                pendingReadError_.clear();
                pendingReadData_.clear();
                sessionAbortReason_.clear();
            }
            appendLog("Browser connected.");
            setStatus(installing_ ? text("status_installing") : text("status_browser_connected"));
            sendJsonEvent("hello", target_ == WebInstallTarget::Nand ? "NAND" : "SD");
            sessionWorker_ = std::thread([this, fd]() { runWebSocketSession(fd); });
            continue;
        }

        if (requestLower.rfind("get / ", 0) == 0 || requestLower.rfind("get /?", 0) == 0 ||
                   requestLower.rfind("get /http", 0) == 0) {
            serveIndexPage(fd);
        } else {
            serveNotFound(fd);
        }
        ::close(fd);
    }

    closeClientSocket();
    closeListenSocket();
    running_ = false;
}

void WebSocketInstallerServer::installWorkerMain(std::vector<RemoteFileEntry> items) {
    std::vector<InstallQueueItem> queue;
    queue.reserve(items.size());
    for (const auto& item : items) {
        InstallQueueItem queueItem;
        queueItem.path = item.virtualPath;
        queueItem.name = item.name;
        queueItem.size = item.size;
        queue.push_back(std::move(queueItem));
    }

    InstallBackendCallbacks callbacks;
    callbacks.onLog = [this](const std::string& line) {
        appendLog(line);
        sendJsonEvent("log", line);
    };
    callbacks.onStatus = [this](const std::string& status) {
        setStatus(status);
        setProgress(currentProgress(), totalProgress(), status);
        sendJsonEvent("status", status);
    };
    callbacks.onProgress = [this](float current, float total) {
        std::string currentItem = this->currentItem();
        setProgress(current, total, currentItem);
        sendProgressEvent(current, total, currentItem);
    };

    InstallDataSourceCallbacks sourceCallbacks;
    sourceCallbacks.readRange =
        [this](const InstallQueueItem& item, uint64_t offset, size_t size, void* outBuffer,
               std::string& errOut) -> bool {
            std::string path = item.path;
            size_t dot = path.rfind('.');
            std::string fileId =
                (dot == std::string::npos || dot <= 4) ? path.substr(4) : path.substr(4, dot - 4);
            return requestRemoteRead(fileId, offset, size, outBuffer, errOut);
        };

    std::string err;
    bool ok = runInstallQueue(queue, target_ == WebInstallTarget::Nand, false,
                              callbacks, err, &sourceCallbacks);

    installing_ = false;
    if (ok) {
        appendLog("Web install completed.");
        setStatus(text("status_install_completed"));
        setProgress(1.0f, 1.0f, "");
        sendInstallResult(true, "Install completed.");
    } else {
        if (!err.empty())
            appendLog("Web install failed: " + err);
        setStatus(text("status_install_failed"));
        sendInstallResult(false, err.empty() ? "Install failed." : err);
    }

    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        pendingReadReady_ = false;
        pendingReadReqId_ = 0;
        pendingReadError_.clear();
        pendingReadData_.clear();
        sessionAbortReason_.clear();
    }
    closeClientSocket();
}

void WebSocketInstallerServer::appendLog(const std::string& line) {
    if (line.empty())
        return;
#ifdef XPLORE_DEBUG
    std::printf("[web-install] %s\n", line.c_str());
#endif
    std::lock_guard<std::mutex> lock(mutex_);
    if (logs_.size() >= kMaxLogs)
        logs_.pop_front();
    logs_.push_back(line);
}

void WebSocketInstallerServer::setStatus(const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = value;
}

void WebSocketInstallerServer::setProgress(float current, float total, const std::string& currentItem) {
    std::lock_guard<std::mutex> lock(mutex_);
    currentProgress_ = current;
    totalProgress_ = total;
    currentItem_ = currentItem;
}

void WebSocketInstallerServer::closeClientSocket() {
    int fd = -1;
    std::string reason;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        sessionActive_ = false;
        fd = clientFd_;
        clientFd_ = -1;
        reason = sessionAbortReason_.empty() ? text("error_browser_disconnected")
                                             : sessionAbortReason_;
    }
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
    abortPendingRead(reason);
}

void WebSocketInstallerServer::closeListenSocket() {
    closeFd(listenFd_);
}

std::string WebSocketInstallerServer::readHttpRequest(int fd) {
    std::string request;
    request.reserve(2048);
    char buffer[512];
    uint64_t startTick = armGetSystemTick();
    uint64_t freq = armGetSystemTickFreq();

    while (!stopRequested_ && request.find("\r\n\r\n") == std::string::npos) {
        uint64_t elapsed = armGetSystemTick() - startTick;
        if (elapsed > freq * 2) {
            appendLog("HTTP request header timeout.");
            break;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval tv {};
        tv.tv_sec = 0;
        tv.tv_usec = 250000;
        int ready = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
        if (ready <= 0)
            continue;

        int n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0)
            break;
        request.append(buffer, static_cast<size_t>(n));
        if (request.size() > 16384)
            break;
    }

    return request;
}

void WebSocketInstallerServer::serveIndexPage(int fd) {
    std::string body = buildIndexHtml();
    std::string header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    sendAll(fd, header.data(), header.size());
    sendAll(fd, body.data(), body.size());
}

void WebSocketInstallerServer::serveNotFound(int fd) {
    static const char kResp[] =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: 10\r\n"
        "Connection: close\r\n"
        "\r\n"
        "Not Found\n";
    sendAll(fd, kResp, sizeof(kResp) - 1);
}

bool WebSocketInstallerServer::tryUpgradeWebSocket(int fd, const std::string& request) {
    std::string key = headerValue(request, "Sec-WebSocket-Key");
    if (key.empty())
        return false;

    std::string accept = websocketAcceptKey(key);
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
    return sendAll(fd, response.data(), response.size());
}

void WebSocketInstallerServer::runWebSocketSession(int fd) {
    while (!stopRequested_) {
        uint8_t opcode = 0;
        std::string payload;
        if (!recvWsFrame(fd, opcode, payload, stopRequested_))
            break;

        if (opcode == 0x8) {
            sendWsFrame(fd, 0x8, nullptr, 0);
            break;
        }
        if (opcode == 0x9) {
            sendWsFrame(fd, 0xA, payload.data(), payload.size());
            continue;
        }
        if (opcode == 0x1) {
            if (!handleClientTextMessage(payload))
                break;
            continue;
        }
        if (opcode == 0x2) {
            if (!handleClientBinaryMessage(payload))
                break;
            continue;
        }
    }

    if (!installing_ && status() == text("status_browser_connected"))
        setStatus(text("status_waiting_browser"));
    closeClientSocket();
}

bool WebSocketInstallerServer::handleClientTextMessage(const std::string& payload) {
    json_object* root = json_tokener_parse(payload.c_str());
    if (!root)
        return true;

    json_object* typeObject = nullptr;
    if (!json_object_object_get_ex(root, "type", &typeObject) ||
        json_object_get_type(typeObject) != json_type_string) {
        json_object_put(root);
        return true;
    }

    std::string type = json_object_get_string(typeObject);
    if (type == "ping") {
        sendJsonEvent("pong", "pong");
        json_object_put(root);
        return true;
    }

    if (type == "install") {
        if (installing_) {
            sendJsonEvent("error", "Install already in progress.");
            json_object_put(root);
            return true;
        }

        json_object* itemsObject = nullptr;
        if (!json_object_object_get_ex(root, "items", &itemsObject) ||
            json_object_get_type(itemsObject) != json_type_array) {
            sendJsonEvent("error", "Invalid install manifest.");
            json_object_put(root);
            return true;
        }

        std::vector<RemoteFileEntry> items;
        uint64_t totalBytes = 0;
        size_t len = json_object_array_length(itemsObject);
        items.reserve(len);
        for (size_t i = 0; i < len; ++i) {
            json_object* itemObject = json_object_array_get_idx(itemsObject, static_cast<int>(i));
            if (!itemObject)
                continue;

            json_object* idObject = nullptr;
            json_object* nameObject = nullptr;
            json_object* sizeObject = nullptr;
            if (!json_object_object_get_ex(itemObject, "id", &idObject) ||
                !json_object_object_get_ex(itemObject, "name", &nameObject) ||
                !json_object_object_get_ex(itemObject, "size", &sizeObject) ||
                json_object_get_type(idObject) != json_type_string ||
                json_object_get_type(nameObject) != json_type_string) {
                sendJsonEvent("error", "Invalid item in manifest.");
                json_object_put(root);
                return true;
            }

            RemoteFileEntry entry;
            entry.id = json_object_get_string(idObject);
            entry.name = json_object_get_string(nameObject);
            entry.size = static_cast<uint64_t>(json_object_get_int64(sizeObject));

            std::string ext = fileExtLower(entry.name);
            if (!isAsciiId(entry.id) || !isSupportedInstallExt(ext) || entry.size == 0) {
                sendJsonEvent("error", "Manifest contains unsupported file.");
                json_object_put(root);
                return true;
            }
            entry.virtualPath = "web:" + entry.id + ext;
            totalBytes += entry.size;
            items.push_back(std::move(entry));
        }

        if (items.empty()) {
            sendJsonEvent("error", "No install items.");
            json_object_put(root);
            return true;
        }

        if (installWorker_.joinable())
            installWorker_.join();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            itemCount_ = items.size();
            totalBytes_ = totalBytes;
            currentProgress_ = 0.0f;
            totalProgress_ = 0.0f;
            currentItem_.clear();
        }
        appendLog("Install request accepted.");
        for (const auto& item : items)
            appendLog("Queued [" + item.id + "] " + item.name);

        installing_ = true;
        setStatus(text("status_preparing_install"));
        sendJsonEvent("status", text("status_preparing_install"));
        installWorker_ = std::thread([this, items = std::move(items)]() mutable {
            installWorkerMain(std::move(items));
        });

        json_object_put(root);
        return true;
    }

    json_object_put(root);
    return true;
}

bool WebSocketInstallerServer::handleClientBinaryMessage(const std::string& payload) {
    if (payload.size() < 12)
        return true;

    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(payload.data());
    if (std::memcmp(ptr, kReadMagic, 4) != 0)
        return true;

    uint32_t reqId = 0;
    uint32_t size = 0;
    parseUint32LE(ptr + 4, reqId);
    parseUint32LE(ptr + 8, size);

    std::lock_guard<std::mutex> lock(sessionMutex_);
    if (!sessionActive_ || reqId != pendingReadReqId_)
        return true;
    if (payload.size() != static_cast<size_t>(12 + size)) {
        appendLog("Remote chunk size mismatch.");
        pendingReadError_ = "Remote chunk size mismatch.";
        pendingReadReady_ = true;
        sessionCv_.notify_all();
        return true;
    }

    pendingReadData_.assign(ptr + 12, ptr + 12 + size);
    pendingReadError_.clear();
    pendingReadReady_ = true;
    sessionCv_.notify_all();
    return true;
}

bool WebSocketInstallerServer::requestRemoteRead(const std::string& fileId, uint64_t offset, size_t size,
                                             void* outBuffer, std::string& errOut) {
    uint32_t reqId = 0;
    {
        std::unique_lock<std::mutex> lock(sessionMutex_);
        if (!sessionActive_ || clientFd_ < 0) {
            errOut = sessionAbortReason_.empty() ? text("error_browser_not_connected")
                                                 : sessionAbortReason_;
            return false;
        }
        pendingReadReqId_ = nextReqId_++;
        pendingReadReady_ = false;
        pendingReadError_.clear();
        pendingReadData_.clear();
        reqId = pendingReadReqId_;
    }

    json_object* root = json_object_new_object();
    jsonAddString(root, "type", "read");
    json_object_object_add(root, "reqId", json_object_new_int64(reqId));
    jsonAddString(root, "fileId", fileId);
    json_object_object_add(root, "offset", json_object_new_int64(static_cast<int64_t>(offset)));
    json_object_object_add(root, "size", json_object_new_int64(static_cast<int64_t>(size)));
    std::string message = jsonString(root);
    json_object_put(root);

    {
        std::lock_guard<std::mutex> sendLock(sendMutex_);
        if (clientFd_ < 0 || !sendWsText(clientFd_, message)) {
            abortPendingRead(text("error_send_read_request"));
            appendLog("Failed to send remote read request.");
            errOut = text("error_send_read_request");
            return false;
        }
    }

    std::unique_lock<std::mutex> lock(sessionMutex_);
    sessionCv_.wait(lock, [this]() {
        return stopRequested_ || pendingReadReady_ || !sessionActive_;
    });

    if (stopRequested_) {
        errOut = "Stopped";
        return false;
    }
    if (!pendingReadReady_) {
        appendLog("Browser disconnected during remote read.");
        errOut = text("error_browser_disconnected");
        return false;
    }
    if (!pendingReadError_.empty()) {
        appendLog("Remote read failed: " + pendingReadError_);
        errOut = pendingReadError_;
        return false;
    }
    if (pendingReadData_.size() != size) {
        appendLog("Remote chunk length mismatch.");
        errOut = "Remote chunk length mismatch.";
        return false;
    }

    std::memcpy(outBuffer, pendingReadData_.data(), size);
    pendingReadReady_ = false;
    pendingReadData_.clear();
    pendingReadError_.clear();
    return true;
}

bool WebSocketInstallerServer::sendJsonEvent(const std::string& type, const std::string& message) {
    json_object* root = json_object_new_object();
    jsonAddString(root, "type", type);
    jsonAddString(root, "message", message);
    std::string payload = jsonString(root);
    json_object_put(root);

    std::lock_guard<std::mutex> sendLock(sendMutex_);
    if (clientFd_ < 0)
        return false;
    return sendWsText(clientFd_, payload);
}

bool WebSocketInstallerServer::sendProgressEvent(float current, float total, const std::string& currentItem) {
    json_object* root = json_object_new_object();
    jsonAddString(root, "type", "progress");
    json_object_object_add(root, "current", json_object_new_double(current));
    json_object_object_add(root, "total", json_object_new_double(total));
    jsonAddString(root, "currentItem", currentItem);
    std::string payload = jsonString(root);
    json_object_put(root);

    std::lock_guard<std::mutex> sendLock(sendMutex_);
    if (clientFd_ < 0)
        return false;
    return sendWsText(clientFd_, payload);
}

bool WebSocketInstallerServer::sendInstallResult(bool ok, const std::string& message) {
    json_object* root = json_object_new_object();
    jsonAddString(root, "type", "result");
    json_object_object_add(root, "ok", json_object_new_boolean(ok));
    jsonAddString(root, "message", message);
    std::string payload = jsonString(root);
    json_object_put(root);

    std::lock_guard<std::mutex> sendLock(sendMutex_);
    if (clientFd_ < 0)
        return false;
    return sendWsText(clientFd_, payload);
}

void WebSocketInstallerServer::abortPendingRead(const std::string& reason) {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    if (pendingReadReady_)
        return;
    pendingReadError_ = reason;
    pendingReadReady_ = true;
    sessionCv_.notify_all();
}

std::string WebSocketInstallerServer::buildIndexHtml() const {
    json_object* textRoot = json_object_new_object();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& pair : textMap_)
            json_object_object_add(textRoot, pair.first.c_str(),
                                   json_object_new_string(pair.second.c_str()));
    }
    std::string textJson = jsonString(textRoot);
    json_object_put(textRoot);

    std::string html = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>__HTML_TITLE__</title>
<style>
:root{
  --bg:#0b1220;
  --panel:#182236;
  --panel2:#22314c;
  --line:#324564;
  --text:#edf4ff;
  --muted:#93a6c5;
  --accent:#67a9ff;
  --accent2:#326fd0;
  --danger:#ef5350;
}
*{box-sizing:border-box}
body{
  margin:0;
  min-height:100vh;
  color:var(--text);
  font-family:ui-sans-serif,system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;
  background:
    radial-gradient(circle at top left,rgba(75,118,196,.35),transparent 28%),
    radial-gradient(circle at bottom right,rgba(36,72,132,.28),transparent 24%),
    linear-gradient(180deg,#09111d 0,#0c1524 100%);
}
.wrap{
  width:min(1080px,calc(100vw - 28px));
  margin:24px auto;
  padding:24px;
  border:1px solid rgba(255,255,255,.08);
  border-radius:24px;
  background:rgba(16,24,39,.92);
  box-shadow:0 28px 80px rgba(0,0,0,.4);
  backdrop-filter:blur(12px);
}
h1{margin:0;font-size:32px}
.sub{margin-top:8px;color:var(--muted)}
.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:16px;margin-top:20px}
.card{
  border:1px solid var(--line);
  border-radius:18px;
  background:linear-gradient(180deg,var(--panel) 0,var(--panel2) 100%);
  padding:18px;
}
.label{font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:var(--muted);margin-bottom:8px}
.value{font-size:18px;word-break:break-all}
.toolbar{display:flex;gap:12px;flex-wrap:wrap;margin-top:16px}
button{
  border:0;
  border-radius:12px;
  padding:12px 18px;
  font-size:15px;
  font-weight:700;
  color:#fff;
  background:linear-gradient(180deg,var(--accent) 0,var(--accent2) 100%);
  cursor:pointer;
}
button.alt{background:#2b3d5b}
button.danger{background:var(--danger)}
button:disabled{opacity:.45;cursor:default}
input[type=file]{display:none}
.queue{
  margin-top:16px;
  border:1px solid #22314c;
  border-radius:18px;
  overflow:hidden;
}
.row{
  display:grid;
  grid-template-columns:140px 1fr 130px;
  gap:16px;
  padding:12px 16px;
  border-top:1px solid rgba(255,255,255,.06);
}
.row:first-child{border-top:0}
.head{background:rgba(255,255,255,.04);color:var(--muted);font-size:13px;font-weight:700}
.mono{font-family:ui-monospace,SFMono-Regular,Menlo,monospace}
.name{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
pre{
  margin:0;
  margin-top:16px;
  min-height:280px;
  max-height:420px;
  overflow:auto;
  padding:16px;
  border-radius:18px;
  background:#09111d;
  border:1px solid #22314c;
  color:#c1d2ee;
  font:14px/1.55 ui-monospace,SFMono-Regular,Menlo,monospace;
}
.progress{
  margin-top:16px;
  display:grid;
  gap:12px;
}
.bar{
  height:18px;
  border-radius:999px;
  overflow:hidden;
  border:1px solid #2b4164;
  background:#101b2d;
}
.fill{
  height:100%;
  width:0%;
  background:linear-gradient(90deg,var(--accent) 0,#84c1ff 100%);
}
@media (max-width:860px){
  .grid{grid-template-columns:1fr}
  .row{grid-template-columns:120px 1fr 96px}
}
</style>
</head>
<body>
<div class="wrap">
  <h1 id="title"></h1>
  <div class="sub" id="sub"></div>
  <div class="grid">
    <div class="card">
      <div class="label" id="label-connection"></div>
      <div class="value" id="conn"></div>
    </div>
    <div class="card">
      <div class="label" id="label-status"></div>
      <div class="value" id="status"></div>
    </div>
  </div>

  <div class="card" style="margin-top:16px">
    <div class="label" id="label-hint"></div>
    <div class="value" id="hint"></div>
  </div>

  <div class="toolbar">
    <input id="file-input" type="file" multiple accept=".nsp,.nsz,.xci,.xcz">
    <button id="pick-btn" type="button"></button>
    <button id="clear-btn" class="alt" type="button"></button>
    <button id="install-btn" type="button"></button>
  </div>

  <div class="queue" id="queue"></div>

  <div class="progress">
    <div class="card">
      <div class="label" id="label-current-package"></div>
      <div class="value" id="current-item">-</div>
      <div class="bar" style="margin-top:10px"><div class="fill" id="bar-current"></div></div>
    </div>
    <div class="card">
      <div class="label" id="label-overall-progress"></div>
      <div class="value" id="overall-text">0%</div>
      <div class="bar" style="margin-top:10px"><div class="fill" id="bar-total"></div></div>
    </div>
  </div>

  <pre id="log"></pre>
</div>
<script>
const TEXT = __I18N_JSON__;
const fileInput = document.getElementById("file-input");
const queueEl = document.getElementById("queue");
const connEl = document.getElementById("conn");
const statusEl = document.getElementById("status");
const hintEl = document.getElementById("hint");
const currentItemEl = document.getElementById("current-item");
const overallTextEl = document.getElementById("overall-text");
const logEl = document.getElementById("log");
const clearBtn = document.getElementById("clear-btn");
const installBtn = document.getElementById("install-btn");
const pickBtn = document.getElementById("pick-btn");
const barCurrent = document.getElementById("bar-current");
const barTotal = document.getElementById("bar-total");

let ws = null;
let installActive = false;
let resultCloseExpected = false;
let connectFailed = false;
let wsEstablished = false;
let seq = 1;
const queue = [];

function t(key) {
  return Object.prototype.hasOwnProperty.call(TEXT, key) ? TEXT[key] : key;
}

function fmt(template, vars = {}) {
  return String(template).replace(/\{([a-zA-Z0-9_]+)\}/g, (_, key) =>
    Object.prototype.hasOwnProperty.call(vars, key) ? String(vars[key]) : ""
  );
}

function applyStaticText() {
  document.title = t("page_title");
  document.getElementById("title").textContent = t("page_title");
  document.getElementById("sub").textContent = t("page_sub");
  document.getElementById("label-connection").textContent = t("page_label_connection");
  document.getElementById("label-status").textContent = t("page_label_status");
  document.getElementById("label-hint").textContent = t("page_label_hint");
  document.getElementById("label-current-package").textContent = t("page_label_current_package");
  document.getElementById("label-overall-progress").textContent = t("page_label_overall_progress");
  pickBtn.textContent = t("page_button_add_files");
  clearBtn.textContent = t("page_button_clear_queue");
  installBtn.textContent = t("page_button_start_install");
  currentItemEl.textContent = "-";
  overallTextEl.textContent = "0%";
}

function fmtBytes(bytes) {
  const units = ["B","KB","MB","GB","TB"];
  let value = bytes;
  let idx = 0;
  while (value >= 1024 && idx < units.length - 1) {
    value /= 1024;
    idx++;
  }
  return `${value.toFixed(idx === 0 ? 0 : 1)} ${units[idx]}`;
}

function log(line) {
  const current = logEl.textContent ? logEl.textContent.split("\n").filter(Boolean) : [];
  current.push(line);
  logEl.textContent = current.slice(-30).join("\n");
  if (logEl.textContent) logEl.textContent += "\n";
  logEl.scrollTop = logEl.scrollHeight;
}

function updateButtons() {
  const connecting = !!ws && ws.readyState === WebSocket.CONNECTING;
  pickBtn.disabled = installActive;
  clearBtn.disabled = installActive || queue.length === 0;
  installBtn.disabled = installActive || queue.length === 0 || connecting;
  fileInput.disabled = installActive;
}

function updateConnectionView() {
  if (ws && ws.readyState === WebSocket.CONNECTING) {
    connEl.textContent = t("page_conn_connecting");
    hintEl.textContent = t("page_hint_connecting");
    return;
  }
  if (ws && ws.readyState === WebSocket.OPEN) {
    connEl.textContent = t("page_conn_active");
    hintEl.textContent = t("page_hint_active");
    return;
  }
  connEl.textContent = t("page_conn_http_ready");
  if (connectFailed) {
    hintEl.textContent = t("page_hint_connect_failed");
  } else {
    hintEl.textContent = t("page_hint_idle");
  }
}

function renderQueue() {
  if (!queue.length) {
    queueEl.innerHTML = `<div class="row"><div class="mono">-</div><div class="name">${t("page_no_queued_files")}</div><div>0 B</div></div>`;
    updateButtons();
    return;
  }
  queueEl.innerHTML = `<div class="row head"><div>${t("page_col_id")}</div><div>${t("page_col_name")}</div><div>${t("page_col_size")}</div></div>` +
    queue.map(item => `<div class="row"><div class="mono">${item.id}</div><div class="name">${item.name}</div><div>${fmtBytes(item.size)}</div></div>`).join("");
  updateButtons();
}

function nextId() {
  const id = `PKG-${String(seq).padStart(3, "0")}`;
  seq += 1;
  return id;
}

pickBtn.addEventListener("click", () => {
  if (pickBtn.disabled) return;
  fileInput.click();
});

fileInput.addEventListener("change", () => {
  for (const file of fileInput.files) {
    const name = file.name || "";
    const lower = name.toLowerCase();
    if (!lower.endsWith(".nsp") && !lower.endsWith(".nsz") &&
        !lower.endsWith(".xci") && !lower.endsWith(".xcz")) {
      log(fmt(t("page_log_skip_unsupported"), { name }));
      continue;
    }
    queue.push({
      id: nextId(),
      name,
      size: file.size,
      file,
    });
  }
  fileInput.value = "";
  renderQueue();
});

clearBtn.addEventListener("click", () => {
  if (installActive) return;
  queue.length = 0;
  renderQueue();
});

function startInstallWebSocket() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  resultCloseExpected = false;
  connectFailed = false;
  wsEstablished = false;
  ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.binaryType = "arraybuffer";
  updateConnectionView();
  updateButtons();

  ws.onopen = () => {
    log(t("page_log_ws_connected"));
    connectFailed = false;
    wsEstablished = true;
    updateConnectionView();
    updateButtons();
    statusEl.textContent = t("page_status_manifest");
    ws.send(JSON.stringify({
      type: "install",
      items: queue.map(item => ({ id: item.id, name: item.name, size: item.size })),
    }));
  };

  ws.onmessage = handleMessage;

  ws.onclose = () => {
    const closingDuringInstall = installActive && !resultCloseExpected;
    const openedBeforeClose = wsEstablished;
    ws = null;
    if (closingDuringInstall)
      connectFailed = true;
    updateConnectionView();
    updateButtons();
    if (closingDuringInstall) {
      installActive = false;
      statusEl.textContent = openedBeforeClose ? t("page_status_connection_lost")
                                               : t("page_status_connect_failed");
      log(openedBeforeClose ? t("page_log_ws_closed_during")
                            : t("page_log_ws_error"));
    } else {
      log(t("page_log_ws_closed"));
    }
    resultCloseExpected = false;
    wsEstablished = false;
  };

  ws.onerror = () => {
    connectFailed = true;
    statusEl.textContent = t("page_status_connect_failed");
    log(t("page_log_ws_error"));
    updateConnectionView();
  };
}

installBtn.addEventListener("click", () => {
  if (installActive || !queue.length) return;
  installActive = true;
  log(fmt(t("page_log_install_request"), { count: queue.length }));
  statusEl.textContent = t("page_status_connecting");
  startInstallWebSocket();
  updateButtons();
});

async function handleReadRequest(message) {
  const item = queue.find(entry => entry.id === message.fileId);
  if (!item) {
    log(fmt(t("page_log_missing_file"), { fileId: message.fileId }));
    return;
  }
  const blob = item.file.slice(Number(message.offset), Number(message.offset) + Number(message.size));
  const data = new Uint8Array(await blob.arrayBuffer());
  const packet = new Uint8Array(12 + data.length);
  packet[0] = 0x58; packet[1] = 0x50; packet[2] = 0x52; packet[3] = 0x44;
  const view = new DataView(packet.buffer);
  view.setUint32(4, Number(message.reqId), true);
  view.setUint32(8, data.length, true);
  packet.set(data, 12);
  ws.send(packet);
}

function handleMessage(event) {
  if (typeof event.data !== "string") {
    log(t("page_log_unexpected_binary"));
    return;
  }
  const msg = JSON.parse(event.data);
  if (msg.type === "hello") {
    connectFailed = false;
    log(fmt(t("page_log_ns_hello"), { target: msg.message || "" }));
    return;
  }
  if (msg.type === "status") {
    statusEl.textContent = msg.message || "";
    return;
  }
  if (msg.type === "log") {
    log(`ns >> ${msg.message}`);
    return;
  }
  if (msg.type === "progress") {
    const current = Math.max(0, Math.min(1, Number(msg.current || 0)));
    const total = Math.max(0, Math.min(1, Number(msg.total || 0)));
    barCurrent.style.width = `${current * 100}%`;
    barTotal.style.width = `${total * 100}%`;
    overallTextEl.textContent = `${(total * 100).toFixed(total < 0.1 ? 2 : 1)}%`;
    currentItemEl.textContent = msg.currentItem || "-";
    return;
  }
  if (msg.type === "result") {
    installActive = false;
    resultCloseExpected = true;
    connectFailed = false;
    statusEl.textContent = msg.ok ? t("page_status_completed") : t("page_status_failed");
    log(fmt(t(msg.ok ? "page_log_ns_success" : "page_log_ns_failed"),
            { message: msg.message || "" }));
    updateButtons();
    return;
  }
  if (msg.type === "error") {
    installActive = false;
    statusEl.textContent = t("page_status_error");
    log(fmt(t("page_log_ns_error"), { message: msg.message || "" }));
    updateButtons();
    return;
  }
  if (msg.type === "read") {
    handleReadRequest(msg);
    return;
  }
}

window.addEventListener("beforeunload", (event) => {
  if (!installActive && (!ws || ws.readyState !== WebSocket.CONNECTING)) return;
  event.preventDefault();
  event.returnValue = "";
});

applyStaticText();
renderQueue();
statusEl.textContent = t("page_status_waiting");
updateConnectionView();
</script>
</body>
</html>
)HTML";
    replaceAll(html, "__HTML_TITLE__", htmlEscape(text("page_title")));
    replaceAll(html, "__I18N_JSON__", textJson);
    return html;
}

std::string WebSocketInstallerServer::websocketAcceptKey(const std::string& clientKey) {
    std::string input = clientKey + kWsGuid;
    uint8_t hash[SHA1_HASH_SIZE];
    sha1CalculateHash(hash, input.data(), input.size());
    return base64Encode(hash, sizeof(hash));
}

std::string WebSocketInstallerServer::trim(const std::string& value) {
    size_t start = 0;
    while (start < value.size() &&
           (value[start] == ' ' || value[start] == '\t' || value[start] == '\r' || value[start] == '\n')) {
        ++start;
    }
    size_t end = value.size();
    while (end > start &&
           (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r' || value[end - 1] == '\n')) {
        --end;
    }
    return value.substr(start, end - start);
}

std::string WebSocketInstallerServer::headerValue(const std::string& request, const char* name) {
    std::string requestLower = toLowerCopy(request);
    std::string pattern = toLowerCopy(std::string(name)) + ":";
    size_t pos = requestLower.find(pattern);
    if (pos == std::string::npos)
        return "";
    pos += pattern.size();
    size_t end = request.find("\r\n", pos);
    if (end == std::string::npos)
        end = request.size();
    return trim(request.substr(pos, end - pos));
}

bool WebSocketInstallerServer::recvExact(int fd, void* buf, size_t size, int timeoutMs,
                         const std::atomic<bool>& stopRequested) {
    uint8_t* out = static_cast<uint8_t*>(buf);
    size_t received = 0;

    while (received < size && !stopRequested) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval tv {};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;

        int ready = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
        if (ready <= 0)
            continue;

        int rc = ::recv(fd, out + received, size - received, 0);
        if (rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return false;
        }
        if (rc == 0)
            return false;
        received += static_cast<size_t>(rc);
    }

    return received == size;
}

bool WebSocketInstallerServer::sendAll(int fd, const void* data, size_t size) {
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    while (sent < size) {
        int rc = ::send(fd, ptr + sent, size - sent, 0);
        if (rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!waitForSocket(fd, true, 2000))
                    return false;
                continue;
            }
            return false;
        }
        if (rc == 0)
            return false;
        sent += static_cast<size_t>(rc);
    }
    return true;
}

bool WebSocketInstallerServer::sendWsFrame(int fd, uint8_t opcode, const void* data, size_t size) {
    uint8_t header[10];
    size_t headerSize = 0;
    header[headerSize++] = static_cast<uint8_t>(0x80 | (opcode & 0x0f));
    if (size <= 125) {
        header[headerSize++] = static_cast<uint8_t>(size);
    } else if (size <= 0xffff) {
        header[headerSize++] = 126;
        header[headerSize++] = static_cast<uint8_t>((size >> 8) & 0xff);
        header[headerSize++] = static_cast<uint8_t>(size & 0xff);
    } else {
        header[headerSize++] = 127;
        for (int i = 7; i >= 0; --i)
            header[headerSize++] = static_cast<uint8_t>((static_cast<uint64_t>(size) >> (i * 8)) & 0xff);
    }

    if (!sendAll(fd, header, headerSize))
        return false;
    if (size == 0)
        return true;
    return sendAll(fd, data, size);
}

bool WebSocketInstallerServer::sendWsText(int fd, const std::string& text) {
    return sendWsFrame(fd, 0x1, text.data(), text.size());
}

bool WebSocketInstallerServer::recvWsFrame(int fd, uint8_t& opcode, std::string& payload,
                                       const std::atomic<bool>& stopRequested) {
    payload.clear();
    opcode = 0;
    bool haveDataOpcode = false;

    while (!stopRequested) {
        uint8_t head[2];
        if (!recvExact(fd, head, sizeof(head), 500, stopRequested))
            return false;

        bool fin = (head[0] & 0x80) != 0;
        uint8_t frameOpcode = head[0] & 0x0f;
        bool masked = (head[1] & 0x80) != 0;
        uint64_t length = head[1] & 0x7f;
        if (length == 126) {
            uint8_t ext[2];
            if (!recvExact(fd, ext, sizeof(ext), 500, stopRequested))
                return false;
            length = (static_cast<uint64_t>(ext[0]) << 8) | static_cast<uint64_t>(ext[1]);
        } else if (length == 127) {
            uint8_t ext[8];
            if (!recvExact(fd, ext, sizeof(ext), 500, stopRequested))
                return false;
            length = 0;
            for (int i = 0; i < 8; ++i)
                length = (length << 8) | static_cast<uint64_t>(ext[i]);
        }

        if (length > 2 * 1024 * 1024)
            return false;

        uint8_t mask[4] = {0, 0, 0, 0};
        if (masked && !recvExact(fd, mask, sizeof(mask), 500, stopRequested))
            return false;

        std::string framePayload(static_cast<size_t>(length), '\0');
        if (length > 0 &&
            !recvExact(fd, framePayload.data(), static_cast<size_t>(length), 500, stopRequested))
            return false;

        if (masked) {
            for (size_t i = 0; i < framePayload.size(); ++i) {
                framePayload[i] =
                    static_cast<char>(static_cast<uint8_t>(framePayload[i]) ^ mask[i % 4]);
            }
        }

        if (frameOpcode == 0x8 || frameOpcode == 0x9 || frameOpcode == 0xA) {
            opcode = frameOpcode;
            payload = std::move(framePayload);
            return true;
        }

        if (!haveDataOpcode) {
            if (frameOpcode != 0x1 && frameOpcode != 0x2)
                return false;
            opcode = frameOpcode;
            haveDataOpcode = true;
        } else if (frameOpcode != 0x0) {
            return false;
        }

        if (payload.size() + framePayload.size() > 2 * 1024 * 1024)
            return false;
        payload += framePayload;

        if (fin)
            return true;
    }

    return false;
}

} // namespace xplore
