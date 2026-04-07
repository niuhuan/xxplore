#include "util/web_install_demo_server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
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

} // namespace

bool WebInstallDemoServer::start() {
    stop();

    uint16_t port = 0;
    if (!bindListenSocket(port))
        return false;

    port_ = port;
    url_ = "http://" + detectLocalIp() + ":" + std::to_string(port_);
    stopRequested_ = false;
    running_ = true;
    setStatus("Waiting for browser...");
    appendLog("HTTP server ready.");
    appendLog(url_);

    worker_ = std::thread([this]() { workerMain(); });
    return true;
}

void WebInstallDemoServer::stop() {
    stopRequested_ = true;
    closeClientSocket();
    closeListenSocket();
    if (worker_.joinable())
        worker_.join();
    running_ = false;
    port_ = 0;
    setStatus("Stopped");
}

std::string WebInstallDemoServer::url() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return url_;
}

std::string WebInstallDemoServer::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

std::vector<std::string> WebInstallDemoServer::logs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<std::string>(logs_.begin(), logs_.end());
}

bool WebInstallDemoServer::bindListenSocket(uint16_t& outPort) {
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

std::string WebInstallDemoServer::detectLocalIp() {
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

void WebInstallDemoServer::workerMain() {
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

        char ipBuf[64] = {0};
        ::inet_ntop(AF_INET, &clientAddr.sin_addr, ipBuf, sizeof(ipBuf));
        appendLog(std::string("Client connected: ") + ipBuf);

        std::string request = readHttpRequest(fd);
        if (request.empty()) {
            appendLog("HTTP request read failed.");
            ::close(fd);
            continue;
        }

        if (request.find("Upgrade: websocket") != std::string::npos &&
            request.find("GET /ws ") != std::string::npos) {
            if (clientFd_ >= 0) {
                static const char kBusyResp[] =
                    "HTTP/1.1 409 Conflict\r\n"
                    "Content-Type: text/plain; charset=utf-8\r\n"
                    "Content-Length: 19\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "WebSocket busy.\r\n";
                sendAll(fd, kBusyResp, sizeof(kBusyResp) - 1);
                ::close(fd);
                continue;
            }

            if (!tryUpgradeWebSocket(fd, request)) {
                ::close(fd);
                continue;
            }

            clientFd_ = fd;
            runWebSocketSession(fd);
            closeClientSocket();
            continue;
        }

        if (request.find("GET / ") == 0 || request.find("GET /HTTP") == 0) {
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

void WebInstallDemoServer::appendLog(const std::string& line) {
    if (line.empty())
        return;
#ifdef XPLORE_DEBUG
    std::printf("[web-demo] %s\n", line.c_str());
#endif
    std::lock_guard<std::mutex> lock(mutex_);
    if (logs_.size() >= kMaxLogs)
        logs_.pop_front();
    logs_.push_back(line);
}

void WebInstallDemoServer::setStatus(const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = value;
}

void WebInstallDemoServer::closeClientSocket() {
    closeFd(clientFd_);
}

void WebInstallDemoServer::closeListenSocket() {
    closeFd(listenFd_);
}

std::string WebInstallDemoServer::readHttpRequest(int fd) {
    std::string request;
    request.reserve(2048);
    char buffer[512];

    while (!stopRequested_ && request.find("\r\n\r\n") == std::string::npos) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval tv {};
        tv.tv_sec = 1;
        tv.tv_usec = 0;
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

void WebInstallDemoServer::serveIndexPage(int fd) {
    std::string body = buildIndexHtml();
    std::string header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    sendAll(fd, header.data(), header.size());
    sendAll(fd, body.data(), body.size());
    appendLog("Served index page.");
}

void WebInstallDemoServer::serveNotFound(int fd) {
    static const char kResp[] =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: 10\r\n"
        "Connection: close\r\n"
        "\r\n"
        "Not Found\n";
    sendAll(fd, kResp, sizeof(kResp) - 1);
}

bool WebInstallDemoServer::tryUpgradeWebSocket(int fd, const std::string& request) {
    std::string key = headerValue(request, "Sec-WebSocket-Key");
    if (key.empty()) {
        appendLog("WebSocket key missing.");
        return false;
    }

    std::string accept = websocketAcceptKey(key);
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
    if (!sendAll(fd, response.data(), response.size()))
        return false;

    appendLog("WebSocket upgraded.");
    setStatus("Browser connected");
    return true;
}

void WebInstallDemoServer::runWebSocketSession(int fd) {
    appendLog("WebSocket session started.");
    while (!stopRequested_) {
        uint8_t opcode = 0;
        std::string payload;
        if (!recvWsFrame(fd, opcode, payload, stopRequested_))
            break;

        if (opcode == 0x8) {
            appendLog("WebSocket close received.");
            sendWsFrame(fd, 0x8, nullptr, 0);
            break;
        }
        if (opcode == 0x9) {
            sendWsFrame(fd, 0xA, payload.data(), payload.size());
            continue;
        }
        if (opcode != 0x1)
            continue;

        appendLog("WS << " + payload);
        if (payload == "ping") {
            sendWsText(fd, "pong");
            appendLog("WS >> pong");
        } else {
            sendWsText(fd, "unknown");
            appendLog("WS >> unknown");
        }
    }

    appendLog("WebSocket session ended.");
    setStatus("Waiting for browser...");
}

std::string WebInstallDemoServer::buildIndexHtml() {
    return R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Xplore Web Install Demo</title>
<style>
:root{
  --bg:#111827;
  --panel:#182236;
  --panel-2:#202d46;
  --line:#2f405f;
  --text:#e5edf8;
  --muted:#8ea0bc;
  --accent:#69a7ff;
  --accent-2:#3978d6;
}
*{box-sizing:border-box}
body{
  margin:0;
  min-height:100vh;
  font-family:ui-sans-serif,system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;
  background:
    radial-gradient(circle at top left,#28446b 0,transparent 28%),
    radial-gradient(circle at bottom right,#1b3557 0,transparent 25%),
    linear-gradient(180deg,#0b1220 0,#101827 100%);
  color:var(--text);
}
.shell{
  width:min(920px,calc(100vw - 32px));
  margin:32px auto;
  padding:24px;
  border:1px solid rgba(255,255,255,.08);
  border-radius:24px;
  background:rgba(18,26,42,.88);
  box-shadow:0 24px 80px rgba(0,0,0,.35);
  backdrop-filter:blur(12px);
}
h1{margin:0 0 10px;font-size:30px}
.sub{color:var(--muted);margin-bottom:20px}
.row{display:flex;gap:16px;flex-wrap:wrap}
.card{
  flex:1 1 320px;
  background:linear-gradient(180deg,var(--panel) 0,var(--panel-2) 100%);
  border:1px solid var(--line);
  border-radius:18px;
  padding:18px;
}
.label{font-size:12px;text-transform:uppercase;letter-spacing:.08em;color:var(--muted);margin-bottom:8px}
.value{font-size:18px;word-break:break-all}
button{
  border:0;
  border-radius:12px;
  padding:12px 18px;
  background:linear-gradient(180deg,var(--accent) 0,var(--accent-2) 100%);
  color:white;
  font-size:15px;
  font-weight:700;
  cursor:pointer;
}
button:disabled{opacity:.45;cursor:default}
pre{
  margin:0;
  min-height:280px;
  max-height:420px;
  overflow:auto;
  padding:16px;
  border-radius:16px;
  background:#0b1220;
  border:1px solid #22314c;
  color:#bcd0ef;
  font:14px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace;
}
</style>
</head>
<body>
  <div class="shell">
    <h1>Xplore Web Install Demo</h1>
    <div class="sub">Debug-only WebSocket demo. Click Ping and wait for the Switch to reply with Pong.</div>
    <div class="row">
      <div class="card">
        <div class="label">HTTP URL</div>
        <div class="value" id="http-url"></div>
      </div>
      <div class="card">
        <div class="label">WebSocket State</div>
        <div class="value" id="state">connecting</div>
      </div>
    </div>
    <div style="height:16px"></div>
    <div class="row">
      <button id="ping-btn" disabled>Ping</button>
    </div>
    <div style="height:16px"></div>
    <pre id="log"></pre>
  </div>
<script>
const stateEl = document.getElementById("state");
const logEl = document.getElementById("log");
const pingBtn = document.getElementById("ping-btn");
const httpUrlEl = document.getElementById("http-url");
httpUrlEl.textContent = window.location.href;

let ws = null;
function log(line){
  const time = new Date().toLocaleTimeString();
  logEl.textContent += `[${time}] ${line}\n`;
  logEl.scrollTop = logEl.scrollHeight;
}
function connect(){
  const proto = location.protocol === "https:" ? "wss" : "ws";
  ws = new WebSocket(`${proto}://${location.host}/ws`);
  stateEl.textContent = "connecting";
  pingBtn.disabled = true;
  ws.onopen = () => {
    stateEl.textContent = "connected";
    pingBtn.disabled = false;
    log("websocket connected");
  };
  ws.onmessage = (event) => {
    log(`ns >> ${event.data}`);
  };
  ws.onclose = () => {
    stateEl.textContent = "closed";
    pingBtn.disabled = true;
    log("websocket closed");
  };
  ws.onerror = () => {
    stateEl.textContent = "error";
    log("websocket error");
  };
}
pingBtn.addEventListener("click", () => {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  log("web >> ping");
  ws.send("ping");
});
connect();
</script>
</body>
</html>
)HTML";
}

std::string WebInstallDemoServer::websocketAcceptKey(const std::string& clientKey) {
    std::string input = clientKey + kWsGuid;
    uint8_t hash[SHA1_HASH_SIZE];
    sha1CalculateHash(hash, input.data(), input.size());
    return base64Encode(hash, sizeof(hash));
}

std::string WebInstallDemoServer::trim(const std::string& value) {
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

std::string WebInstallDemoServer::headerValue(const std::string& request, const char* name) {
    std::string pattern = std::string(name) + ":";
    size_t pos = request.find(pattern);
    if (pos == std::string::npos)
        return "";
    pos += pattern.size();
    size_t end = request.find("\r\n", pos);
    if (end == std::string::npos)
        end = request.size();
    return trim(request.substr(pos, end - pos));
}

bool WebInstallDemoServer::recvExact(int fd, void* buf, size_t size, int timeoutMs,
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
        if (rc <= 0)
            return false;
        received += static_cast<size_t>(rc);
    }

    return received == size;
}

bool WebInstallDemoServer::sendAll(int fd, const void* data, size_t size) {
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    while (sent < size) {
        int rc = ::send(fd, ptr + sent, size - sent, 0);
        if (rc <= 0)
            return false;
        sent += static_cast<size_t>(rc);
    }
    return true;
}

bool WebInstallDemoServer::sendWsFrame(int fd, uint8_t opcode, const void* data, size_t size) {
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

bool WebInstallDemoServer::sendWsText(int fd, const std::string& text) {
    return sendWsFrame(fd, 0x1, text.data(), text.size());
}

bool WebInstallDemoServer::recvWsFrame(int fd, uint8_t& opcode, std::string& payload,
                                       const std::atomic<bool>& stopRequested) {
    uint8_t head[2];
    if (!recvExact(fd, head, sizeof(head), 500, stopRequested))
        return false;

    opcode = head[0] & 0x0f;
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

    if (length > 64 * 1024)
        return false;

    uint8_t mask[4] = {0, 0, 0, 0};
    if (masked && !recvExact(fd, mask, sizeof(mask), 500, stopRequested))
        return false;

    payload.assign(static_cast<size_t>(length), '\0');
    if (length > 0 && !recvExact(fd, payload.data(), static_cast<size_t>(length), 500, stopRequested))
        return false;

    if (masked) {
        for (size_t i = 0; i < payload.size(); ++i)
            payload[i] = static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask[i % 4]);
    }

    return true;
}

} // namespace xplore
