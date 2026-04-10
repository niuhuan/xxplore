#include "fs/ftp_provider.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <netdb.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace xxplore {
namespace fs {

namespace {

constexpr int kSocketTimeoutSec = 15;
constexpr size_t kSocketBufferSize = 128 * 1024;

void debugLog(const char* fmt, ...) {
#ifdef XXPLORE_DEBUG
    std::va_list args;
    va_start(args, fmt);
    std::printf("[ftp] ");
    std::vprintf(fmt, args);
    std::printf("\n");
    va_end(args);
#else
    (void)fmt;
#endif
}

struct FtpEndpoint {
    std::string host;
    std::string port = "21";
    std::string basePath = "/";
};

struct FtpFeatures {
    bool epsv = false;
    bool mlsd = false;
    bool utf8 = false;
    bool size = false;
    bool restStream = false;
};

struct PassiveDataTarget {
    std::string host;
    int port = 0;
};

struct FtpResponse {
    int code = 0;
    std::vector<std::string> lines;
};

std::string responseMessage(const FtpResponse& response) {
    if (!response.lines.empty())
        return response.lines.back();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "FTP %d", response.code);
    return buf;
}

bool setSocketOptions(int fd) {
    if (fd < 0)
        return false;

    timeval tv {};
    tv.tv_sec = kSocketTimeoutSec;
    tv.tv_usec = 0;
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0)
        return false;
    if (::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0)
        return false;
    int bufSize = static_cast<int>(kSocketBufferSize);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufSize, sizeof(bufSize));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufSize, sizeof(bufSize));
    return true;
}

bool sendAll(int fd, const void* data, size_t size, std::string& errOut) {
    const auto* ptr = static_cast<const unsigned char*>(data);
    size_t sent = 0;
    while (sent < size) {
        int rc = ::send(fd, ptr + sent, size - sent, 0);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            errOut = std::strerror(errno);
            return false;
        }
        if (rc == 0) {
            errOut = "socket closed";
            return false;
        }
        sent += static_cast<size_t>(rc);
    }
    return true;
}

bool recvLine(int fd, std::string& pending, std::string& lineOut, std::string& errOut) {
    while (true) {
        std::size_t newline = pending.find('\n');
        if (newline != std::string::npos) {
            lineOut = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!lineOut.empty() && lineOut.back() == '\r')
                lineOut.pop_back();
            return true;
        }

        char buffer[1024];
        int rc = ::recv(fd, buffer, sizeof(buffer), 0);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            errOut = std::strerror(errno);
            return false;
        }
        if (rc == 0) {
            errOut = "connection closed";
            return false;
        }
        pending.append(buffer, static_cast<size_t>(rc));
        if (pending.size() > 64 * 1024) {
            errOut = "FTP response too large";
            return false;
        }
    }
}

bool readResponse(int fd, std::string& pending, FtpResponse& response, std::string& errOut) {
    response = {};
    std::string line;
    if (!recvLine(fd, pending, line, errOut))
        return false;
    if (line.size() < 3 || !std::isdigit(static_cast<unsigned char>(line[0])) ||
        !std::isdigit(static_cast<unsigned char>(line[1])) ||
        !std::isdigit(static_cast<unsigned char>(line[2]))) {
        errOut = "invalid FTP response";
        return false;
    }

    response.code = std::atoi(line.substr(0, 3).c_str());
    response.lines.push_back(line);

    if (line.size() > 3 && line[3] == '-') {
        std::string terminator = line.substr(0, 3) + " ";
        while (true) {
            if (!recvLine(fd, pending, line, errOut))
                return false;
            response.lines.push_back(line);
            if (line.rfind(terminator, 0) == 0)
                break;
        }
    }
    return true;
}

std::string trim(const std::string& value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])))
        ++start;
    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])))
        --end;
    return value.substr(start, end - start);
}

std::string normalizeAbsolutePath(const std::string& path) {
    std::vector<std::string> parts;
    std::string current;
    auto flush = [&]() {
        if (current.empty() || current == ".") {
            current.clear();
            return;
        }
        if (current == "..") {
            if (!parts.empty())
                parts.pop_back();
            current.clear();
            return;
        }
        parts.push_back(current);
        current.clear();
    };

    for (char ch : path) {
        if (ch == '/') {
            flush();
        } else {
            current.push_back(ch);
        }
    }
    flush();

    if (parts.empty())
        return "/";

    std::string out;
    for (const auto& part : parts) {
        out.push_back('/');
        out += part;
    }
    return out;
}

std::string joinRemotePath(const std::string& basePath, const std::string& relativePath) {
    if (relativePath.empty() || relativePath == "/")
        return normalizeAbsolutePath(basePath);

    if (!relativePath.empty() && relativePath.front() == '/')
        return normalizeAbsolutePath(basePath + relativePath);

    return normalizeAbsolutePath(basePath + "/" + relativePath);
}

std::string baseName(const std::string& path) {
    if (path.empty() || path == "/")
        return {};
    std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return path;
    return path.substr(slash + 1);
}

std::string dirName(const std::string& path) {
    if (path.empty() || path == "/")
        return "/";
    std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0)
        return "/";
    return path.substr(0, slash);
}

bool parseFtpAddress(const std::string& address, FtpEndpoint& endpoint, std::string& errOut) {
    endpoint = {};
    endpoint.port = "21";
    endpoint.basePath = "/";

    std::string value = trim(address);
    if (value.empty()) {
        errOut = "empty FTP address";
        return false;
    }
    if (value.rfind("ftp://", 0) == 0)
        value.erase(0, 6);

    std::size_t slash = value.find('/');
    std::string authority = slash == std::string::npos ? value : value.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : value.substr(slash);
    if (authority.empty()) {
        errOut = "FTP host is missing";
        return false;
    }

    std::size_t at = authority.rfind('@');
    if (at != std::string::npos)
        authority.erase(0, at + 1);

    if (!authority.empty() && authority.front() == '[') {
        std::size_t end = authority.find(']');
        if (end == std::string::npos) {
            errOut = "invalid FTP IPv6 address";
            return false;
        }
        endpoint.host = authority.substr(1, end - 1);
        if (end + 1 < authority.size()) {
            if (authority[end + 1] != ':') {
                errOut = "invalid FTP port";
                return false;
            }
            endpoint.port = authority.substr(end + 2);
        }
    } else {
        std::size_t colon = authority.rfind(':');
        if (colon != std::string::npos && authority.find(':') == colon) {
            endpoint.host = authority.substr(0, colon);
            endpoint.port = authority.substr(colon + 1);
        } else {
            endpoint.host = authority;
        }
    }

    if (endpoint.host.empty()) {
        errOut = "FTP host is missing";
        return false;
    }
    if (endpoint.port.empty())
        endpoint.port = "21";
    if (!std::all_of(endpoint.port.begin(), endpoint.port.end(),
                     [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
        errOut = "invalid FTP port";
        return false;
    }

    endpoint.basePath = normalizeAbsolutePath(path.empty() ? "/" : path);
    return true;
}

bool validateCommandArgument(const std::string& value, std::string& errOut) {
    if (value.find('\r') != std::string::npos || value.find('\n') != std::string::npos) {
        errOut = "invalid FTP path";
        return false;
    }
    return true;
}

bool connectTcp(const std::string& host, const std::string& port, int& fdOut,
                std::string& errOut, std::string* peerHostOut = nullptr) {
    fdOut = -1;
    addrinfo hints {};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    addrinfo* result = nullptr;
    int rc = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &result);
    if (rc != 0) {
        errOut = gai_strerror(rc);
        return false;
    }

    std::string lastErr = "connect failed";
    for (addrinfo* it = result; it; it = it->ai_next) {
        int fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0)
            continue;
        if (!setSocketOptions(fd)) {
            lastErr = std::strerror(errno);
            ::close(fd);
            continue;
        }
        if (::connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            fdOut = fd;
            if (peerHostOut) {
                char hostBuf[NI_MAXHOST];
                if (::getnameinfo(it->ai_addr, it->ai_addrlen, hostBuf, sizeof(hostBuf), nullptr,
                                  0, NI_NUMERICHOST) == 0) {
                    *peerHostOut = hostBuf;
                } else {
                    peerHostOut->clear();
                }
            }
            ::freeaddrinfo(result);
            return true;
        }
        lastErr = std::strerror(errno);
        ::close(fd);
    }

    ::freeaddrinfo(result);
    errOut = lastErr;
    return false;
}

bool parseEpsvPort(const std::string& line, int& portOut) {
    std::size_t left = line.find('(');
    std::size_t right = line.find(')', left == std::string::npos ? 0 : left + 1);
    if (left == std::string::npos || right == std::string::npos || right <= left + 4)
        return false;

    std::string body = line.substr(left + 1, right - left - 1);
    char delimiter = body.front();
    if (body.size() < 5 || body[1] != delimiter || body[2] != delimiter)
        return false;

    std::size_t last = body.find_last_of(delimiter);
    if (last == std::string::npos || last <= 3)
        return false;
    std::size_t prev = body.find_last_of(delimiter, last - 1);
    if (prev == std::string::npos || prev < 2)
        return false;

    std::string portStr = body.substr(prev + 1, last - prev - 1);
    if (portStr.empty())
        return false;
    portOut = std::atoi(portStr.c_str());
    return portOut > 0 && portOut <= 65535;
}

bool parsePasvPort(const std::string& line, std::string& hostOut, int& portOut) {
    std::size_t left = line.find('(');
    std::size_t right = line.find(')', left == std::string::npos ? 0 : left + 1);
    if (left == std::string::npos || right == std::string::npos)
        return false;
    unsigned a = 0, b = 0, c = 0, d = 0, p1 = 0, p2 = 0;
    if (std::sscanf(line.c_str() + left + 1, "%u,%u,%u,%u,%u,%u",
                    &a, &b, &c, &d, &p1, &p2) != 6)
        return false;
    char hostBuf[64];
    std::snprintf(hostBuf, sizeof(hostBuf), "%u.%u.%u.%u", a, b, c, d);
    hostOut = hostBuf;
    portOut = static_cast<int>((p1 << 8) | p2);
    return portOut > 0 && portOut <= 65535;
}

bool parseMlsdLine(const std::string& line, FileEntry& entryOut) {
    std::size_t sep = line.find(' ');
    if (sep == std::string::npos)
        return false;

    std::string facts = line.substr(0, sep);
    std::string name = line.substr(sep + 1);
    if (name.empty() || name == "." || name == "..")
        return false;

    FileEntry entry;
    entry.name = name;

    std::size_t pos = 0;
    while (pos < facts.size()) {
        std::size_t next = facts.find(';', pos);
        std::string fact = facts.substr(pos, next == std::string::npos ? std::string::npos
                                                                        : next - pos);
        std::size_t eq = fact.find('=');
        if (eq != std::string::npos) {
            std::string key = fact.substr(0, eq);
            std::string value = fact.substr(eq + 1);
            for (char& ch : key)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (key == "type") {
                entry.isDirectory = value == "dir" || value == "cdir" || value == "pdir";
            } else if (key == "size") {
                entry.size = static_cast<uint64_t>(std::strtoull(value.c_str(), nullptr, 10));
            }
        }
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }

    if (entry.name == "." || entry.name == "..")
        return false;
    entryOut = std::move(entry);
    return true;
}

bool parseListLine(const std::string& input, FileEntry& entryOut) {
    if (input.empty())
        return false;

    std::string line = input;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
    if (line.empty())
        return false;

    FileEntry entry;

    if (std::isdigit(static_cast<unsigned char>(line[0]))) {
        int month = 0;
        int day = 0;
        int year = 0;
        char timeField[32] = {0};
        char kind[32] = {0};
        char name[512] = {0};
        int parsed = std::sscanf(line.c_str(), "%d-%d-%d %31s %31s %511[^\r\n]",
                                 &month, &day, &year, timeField, kind, name);
        if (parsed == 6) {
            entry.name = name;
            entry.isDirectory = std::strcmp(kind, "<DIR>") == 0;
            if (!entry.isDirectory)
                entry.size = static_cast<uint64_t>(std::strtoull(kind, nullptr, 10));
            if (!entry.name.empty() && entry.name != "." && entry.name != "..") {
                entryOut = std::move(entry);
                return true;
            }
        }
    }

    char perms[32] = {0};
    char owner[128] = {0};
    char group[128] = {0};
    char month[16] = {0};
    char yearOrTime[32] = {0};
    char name[512] = {0};
    unsigned long long size = 0;
    int links = 0;
    int day = 0;
    int parsed =
        std::sscanf(line.c_str(), "%31s %d %127s %127s %llu %15s %d %31s %511[^\r\n]",
                    perms, &links, owner, group, &size, month, &day, yearOrTime, name);
    if (parsed < 9 || name[0] == '\0')
        return false;

    entry.name = name;
    std::size_t arrow = entry.name.find(" -> ");
    if (arrow != std::string::npos)
        entry.name.erase(arrow);
    entry.isDirectory = perms[0] == 'd';
    entry.size = size;
    if (entry.name.empty() || entry.name == "." || entry.name == "..")
        return false;

    entryOut = std::move(entry);
    return true;
}

class FtpSession {
public:
    FtpSession(FtpEndpoint endpoint, std::string user, std::string pass)
        : endpoint_(std::move(endpoint)), user_(std::move(user)), pass_(std::move(pass)) {}

    ~FtpSession() { close(); }

    bool open(std::string& errOut) {
        close();
        if (!connectTcp(endpoint_.host, endpoint_.port, controlFd_, errOut, &peerHost_))
            return false;

        FtpResponse response;
        if (!readResponse(controlFd_, controlPending_, response, errOut)) {
            close();
            return false;
        }
        if (response.code / 100 != 2) {
            errOut = responseMessage(response);
            close();
            return false;
        }

        if (!login(errOut)) {
            close();
            return false;
        }
        if (!detectFeatures(errOut)) {
            close();
            return false;
        }
        if (endpoint_.basePath != "/") {
            FtpResponse response;
            if (!command("CWD " + endpoint_.basePath, response, errOut)) {
                close();
                return false;
            }
            if (response.code / 100 != 2) {
                errOut = responseMessage(response);
                close();
                return false;
            }
        }
        debugLog("connected host=%s port=%s base=%s", endpoint_.host.c_str(),
                 endpoint_.port.c_str(), endpoint_.basePath.c_str());
        return true;
    }

    void close() {
        if (controlFd_ >= 0) {
            ::shutdown(controlFd_, SHUT_RDWR);
            ::close(controlFd_);
            controlFd_ = -1;
        }
        controlPending_.clear();
    }

    bool listDirectory(const std::string& remotePath, std::vector<FileEntry>& entriesOut,
                       std::string& errOut) {
        entriesOut.clear();
        int dataFd = -1;
        bool usedMlsd = false;
        if (!openListData(remotePath, usedMlsd, dataFd, errOut))
            return false;

        std::string listing;
        bool ok = readAllData(dataFd, listing, errOut);
        if (!finishDataTransfer(dataFd, errOut))
            ok = false;
        if (!ok)
            return false;

        std::size_t start = 0;
        while (start < listing.size()) {
            std::size_t end = listing.find('\n', start);
            std::string line =
                listing.substr(start, end == std::string::npos ? std::string::npos : end - start);
            while (!line.empty() && line.back() == '\r')
                line.pop_back();

            FileEntry entry;
            bool parsed = usedMlsd ? parseMlsdLine(line, entry) : parseListLine(line, entry);
            if (parsed)
                entriesOut.push_back(std::move(entry));

            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        return true;
    }

    bool createDirectory(const std::string& remotePath, std::string& errOut) {
        std::string arg = commandPath(remotePath);
        if (arg.empty()) {
            errOut = "invalid FTP path";
            return false;
        }
        return expectCommand("MKD " + arg, 2, errOut);
    }

    bool removeDirectory(const std::string& remotePath, std::string& errOut) {
        std::string arg = commandPath(remotePath);
        if (arg.empty()) {
            errOut = "invalid FTP path";
            return false;
        }
        return expectCommand("RMD " + arg, 2, errOut);
    }

    bool deleteFile(const std::string& remotePath, std::string& errOut) {
        std::string arg = commandPath(remotePath);
        if (arg.empty()) {
            errOut = "invalid FTP path";
            return false;
        }
        return expectCommand("DELE " + arg, 2, errOut);
    }

    bool renamePath(const std::string& from, const std::string& to, std::string& errOut) {
        std::string fromArg = commandPath(from);
        std::string toArg = commandPath(to);
        if (fromArg.empty() || toArg.empty()) {
            errOut = "invalid FTP path";
            return false;
        }
        FtpResponse response;
        if (!command("RNFR " + fromArg, response, errOut))
            return false;
        if (response.code / 100 != 3) {
            errOut = responseMessage(response);
            return false;
        }
        if (!command("RNTO " + toArg, response, errOut))
            return false;
        if (response.code / 100 != 2) {
            errOut = responseMessage(response);
            return false;
        }
        return true;
    }

    bool openReadData(const std::string& remotePath, uint64_t offset, int& dataFdOut,
                      std::string& errOut) {
        dataFdOut = -1;
        std::string pathArg = commandPath(remotePath);
        if (pathArg.empty()) {
            errOut = "invalid FTP path";
            return false;
        }
        if (!setBinaryMode(errOut))
            return false;
        PassiveDataTarget target;
        if (!preparePassiveData(target, errOut))
            return false;
        if (offset > 0) {
            FtpResponse response;
            if (!command("REST " + std::to_string(offset), response, errOut)) {
                return false;
            }
            if (response.code / 100 != 3) {
                errOut = responseMessage(response);
                return false;
            }
        }
        if (!sendCommandOnly("RETR " + pathArg, errOut))
            return false;
        if (!connectPassiveData(target, dataFdOut, errOut))
            return false;
        FtpResponse response;
        if (!readCommandResponse(response, errOut)) {
            ::close(dataFdOut);
            dataFdOut = -1;
            return false;
        }
        if (response.code / 100 != 1) {
            errOut = responseMessage(response);
            ::close(dataFdOut);
            dataFdOut = -1;
            return false;
        }
        return true;
    }

    bool openWriteData(const std::string& remotePath, uint64_t offset, bool truncate,
                       int& dataFdOut, std::string& errOut) {
        dataFdOut = -1;
        std::string pathArg = commandPath(remotePath);
        if (pathArg.empty()) {
            errOut = "invalid FTP path";
            return false;
        }
        if (!setBinaryMode(errOut))
            return false;
        PassiveDataTarget target;
        if (!preparePassiveData(target, errOut))
            return false;
        if (!truncate && offset > 0) {
            FtpResponse response;
            if (!command("REST " + std::to_string(offset), response, errOut)) {
                return false;
            }
            if (response.code / 100 != 3) {
                errOut = responseMessage(response);
                return false;
            }
        }
        if (!sendCommandOnly("STOR " + pathArg, errOut))
            return false;
        if (!connectPassiveData(target, dataFdOut, errOut))
            return false;
        FtpResponse response;
        if (!readCommandResponse(response, errOut)) {
            ::close(dataFdOut);
            dataFdOut = -1;
            return false;
        }
        if (response.code / 100 != 1) {
            errOut = responseMessage(response);
            ::close(dataFdOut);
            dataFdOut = -1;
            return false;
        }
        return true;
    }

    bool finishDataTransfer(int& dataFd, std::string& errOut) {
        if (dataFd >= 0) {
            ::shutdown(dataFd, SHUT_RDWR);
            ::close(dataFd);
            dataFd = -1;
        }
        FtpResponse response;
        if (!readResponse(controlFd_, controlPending_, response, errOut))
            return false;
        if (response.code / 100 != 2) {
            errOut = responseMessage(response);
            return false;
        }
        return true;
    }

private:
    std::string commandPath(const std::string& remotePath) const {
        std::string path = normalizeAbsolutePath(remotePath);
        std::string base = normalizeAbsolutePath(endpoint_.basePath);
        if (path == "/" || path == base)
            return {};
        if (base != "/") {
            std::string prefix = base + "/";
            if (path.rfind(prefix, 0) == 0)
                return path.substr(prefix.size());
        }
        if (!path.empty() && path.front() == '/')
            return path.substr(1);
        return path;
    }

    bool sendCommandOnly(const std::string& cmd, std::string& errOut) {
        if (!validateCommandArgument(cmd, errOut))
            return false;
        std::string wire = cmd;
        wire += "\r\n";
        debugLog("cmd > %s", cmd.c_str());
        return sendAll(controlFd_, wire.data(), wire.size(), errOut);
    }

    bool readCommandResponse(FtpResponse& response, std::string& errOut) {
        bool ok = readResponse(controlFd_, controlPending_, response, errOut);
        if (ok) {
            for (const auto& line : response.lines)
                debugLog("rsp < %s", line.c_str());
        }
        return ok;
    }

    bool command(const std::string& cmd, FtpResponse& response, std::string& errOut) {
        if (!sendCommandOnly(cmd, errOut))
            return false;
        return readCommandResponse(response, errOut);
    }

    bool expectCommand(const std::string& cmd, int klass, std::string& errOut) {
        FtpResponse response;
        if (!command(cmd, response, errOut))
            return false;
        if (response.code / 100 != klass) {
            errOut = responseMessage(response);
            return false;
        }
        return true;
    }

    bool login(std::string& errOut) {
        FtpResponse response;
        std::string user = user_.empty() ? "anonymous" : user_;
        std::string pass = user_.empty() ? "xxplore@" : pass_;

        if (!command("USER " + user, response, errOut))
            return false;
        if (response.code == 230)
            return true;
        if (response.code / 100 != 3) {
            errOut = responseMessage(response);
            return false;
        }

        if (!command("PASS " + pass, response, errOut))
            return false;
        if (response.code / 100 != 2) {
            errOut = responseMessage(response);
            return false;
        }
        return true;
    }

    bool detectFeatures(std::string& errOut) {
        FtpResponse response;
        if (!command("FEAT", response, errOut))
            return false;

        if (response.code / 100 == 2) {
            for (const auto& line : response.lines) {
                if (line.size() <= 4)
                    continue;
                std::string text = trim(line.substr(4));
                for (char& ch : text)
                    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                if (text == "EPSV")
                    features_.epsv = true;
                else if (text == "MLSD")
                    features_.mlsd = true;
                else if (text == "UTF8")
                    features_.utf8 = true;
                else if (text == "SIZE")
                    features_.size = true;
                else if (text == "MLST")
                    features_.mlsd = true;
                else if (text == "REST STREAM")
                    features_.restStream = true;
            }
        }
        errOut.clear();

        if (features_.utf8) {
            std::string utf8Err;
            FtpResponse utf8Response;
            if (!command("OPTS UTF8 ON", utf8Response, utf8Err))
                return false;
            if (utf8Response.code / 100 != 2)
                debugLog("OPTS UTF8 ON rejected: %s", responseMessage(utf8Response).c_str());
        }
        return true;
    }

    bool setBinaryMode(std::string& errOut) { return expectCommand("TYPE I", 2, errOut); }

    bool setAsciiMode(std::string& errOut) { return expectCommand("TYPE A", 2, errOut); }

    bool preparePassiveData(PassiveDataTarget& targetOut, std::string& errOut) {
        targetOut = {};
        std::string epsvErr;
        if (preparePassiveDataEpsv(targetOut, epsvErr))
            return true;
        if (preparePassiveDataPasv(targetOut, errOut))
            return true;
        errOut = epsvErr.empty() ? errOut : epsvErr + " / " + errOut;
        return false;
    }

    bool connectPassiveData(const PassiveDataTarget& target, int& dataFdOut, std::string& errOut) {
        dataFdOut = -1;
        debugLog("data connect host=%s port=%d", target.host.c_str(), target.port);
        return connectTcp(target.host, std::to_string(target.port), dataFdOut, errOut, nullptr);
    }

    bool preparePassiveDataEpsv(PassiveDataTarget& targetOut, std::string& errOut) {
        targetOut = {};
        FtpResponse response;
        if (!command("EPSV", response, errOut))
            return false;
        if (response.code != 229) {
            errOut = responseMessage(response);
            return false;
        }

        int port = 0;
        if (!parseEpsvPort(responseMessage(response), port)) {
            errOut = "invalid EPSV response";
            return false;
        }
        targetOut.host = endpoint_.host;
        targetOut.port = port;
        return true;
    }

    bool preparePassiveDataPasv(PassiveDataTarget& targetOut, std::string& errOut) {
        targetOut = {};
        FtpResponse response;
        if (!command("PASV", response, errOut))
            return false;
        if (response.code != 227) {
            errOut = responseMessage(response);
            return false;
        }

        std::string advertisedHost;
        int port = 0;
        if (!parsePasvPort(responseMessage(response), advertisedHost, port)) {
            errOut = "invalid PASV response";
            return false;
        }
        (void)advertisedHost;
        targetOut.host = endpoint_.host;
        targetOut.port = port;
        return true;
    }

    bool openListData(const std::string& remotePath, bool& usedMlsdOut, int& dataFdOut,
                      std::string& errOut) {
        usedMlsdOut = false;
        dataFdOut = -1;
        if (!setAsciiMode(errOut))
            return false;
        std::string pathArg = commandPath(remotePath);

        auto tryCommand = [&](const std::string& cmd, bool usedMlsd) -> bool {
            PassiveDataTarget target;
            std::string dataErr;
            if (!preparePassiveData(target, dataErr)) {
                debugLog("list prepare failed cmd=%s err=%s", cmd.c_str(), dataErr.c_str());
                return false;
            }
            if (!sendCommandOnly(cmd, dataErr))
                return false;
            int dataFd = -1;
            if (!connectPassiveData(target, dataFd, dataErr))
                return false;
            FtpResponse response;
            if (!readCommandResponse(response, dataErr)) {
                ::close(dataFd);
                return false;
            }
            if (response.code / 100 != 1) {
                ::close(dataFd);
                dataErr = responseMessage(response);
                debugLog("list command rejected cmd=%s err=%s", cmd.c_str(), dataErr.c_str());
                return false;
            }
            dataFdOut = dataFd;
            usedMlsdOut = usedMlsd;
            return true;
        };

        std::string mlsdCmd = pathArg.empty() ? "MLSD" : "MLSD " + pathArg;
        if (tryCommand(mlsdCmd, true))
            return true;
        std::string listCmd = pathArg.empty() ? "LIST" : "LIST " + pathArg;
        if (tryCommand(listCmd, false))
            return true;

        errOut = "FTP listing failed";
        return false;
    }

    bool readAllData(int dataFd, std::string& out, std::string& errOut) {
        out.clear();
        char buffer[8192];
        while (true) {
            int rc = ::recv(dataFd, buffer, sizeof(buffer), 0);
            if (rc < 0) {
                if (errno == EINTR)
                    continue;
                errOut = std::strerror(errno);
                return false;
            }
            if (rc == 0)
                break;
            out.append(buffer, static_cast<size_t>(rc));
        }
        return true;
    }

    FtpEndpoint endpoint_;
    std::string user_;
    std::string pass_;
    int controlFd_ = -1;
    std::string controlPending_;
    std::string peerHost_;
    FtpFeatures features_;
};

bool lookupRemoteEntry(FtpSession& session, const std::string& remotePath, const std::string& basePath,
                       FileStatInfo& statOut, std::string& errOut) {
    statOut = {};
    if (remotePath == normalizeAbsolutePath(basePath)) {
        statOut.exists = true;
        statOut.isDirectory = true;
        return true;
    }

    std::vector<FileEntry> entries;
    std::string parent = dirName(remotePath);
    if (!session.listDirectory(parent, entries, errOut))
        return false;

    std::string wanted = baseName(remotePath);
    for (const auto& entry : entries) {
        if (entry.name != wanted)
            continue;
        statOut.exists = true;
        statOut.isDirectory = entry.isDirectory;
        statOut.size = entry.size;
        return true;
    }
    statOut.exists = false;
    return true;
}

bool removeRemoteRecursive(FtpSession& session, const std::string& remotePath,
                           const std::string& basePath, std::string& errOut) {
    FileStatInfo stat;
    if (!lookupRemoteEntry(session, remotePath, basePath, stat, errOut))
        return false;
    if (!stat.exists) {
        errOut = "FTP path not found";
        return false;
    }
    if (!stat.isDirectory)
        return session.deleteFile(remotePath, errOut);

    std::vector<FileEntry> children;
    if (!session.listDirectory(remotePath, children, errOut))
        return false;
    for (const auto& child : children) {
        if (!removeRemoteRecursive(session, normalizeAbsolutePath(remotePath + "/" + child.name),
                                   basePath, errOut)) {
            return false;
        }
    }
    return session.removeDirectory(remotePath, errOut);
}

class FtpSequentialReader final : public SequentialFileReader {
public:
    FtpSequentialReader(FtpEndpoint endpoint, std::string user, std::string pass,
                        std::string remotePath, uint64_t offset)
        : endpoint_(std::move(endpoint)),
          user_(std::move(user)),
          pass_(std::move(pass)),
          remotePath_(std::move(remotePath)),
          offset_(offset) {}

    ~FtpSequentialReader() override { close(); }

    bool open(std::string& errOut) {
        close();
        session_ = std::make_unique<FtpSession>(endpoint_, user_, pass_);
        if (!session_->open(errOut)) {
            session_.reset();
            return false;
        }
        if (!session_->openReadData(remotePath_, offset_, dataFd_, errOut)) {
            close();
            return false;
        }
        return true;
    }

    bool read(void* outBuffer, size_t size, std::string& errOut) override {
        if (size == 0)
            return true;
        if (dataFd_ < 0) {
            errOut = "FTP stream is closed";
            return false;
        }

        auto* ptr = static_cast<unsigned char*>(outBuffer);
        size_t received = 0;
        while (received < size) {
            int rc = ::recv(dataFd_, ptr + received, size - received, 0);
            if (rc < 0) {
                if (errno == EINTR)
                    continue;
                errOut = std::strerror(errno);
                return false;
            }
            if (rc == 0) {
                errOut = "unexpected eof";
                return false;
            }
            received += static_cast<size_t>(rc);
        }
        return true;
    }

private:
    void close() {
        if (dataFd_ >= 0) {
            ::shutdown(dataFd_, SHUT_RDWR);
            ::close(dataFd_);
            dataFd_ = -1;
        }
        if (session_) {
            session_->close();
            session_.reset();
        }
    }

    FtpEndpoint endpoint_;
    std::string user_;
    std::string pass_;
    std::string remotePath_;
    uint64_t offset_ = 0;
    std::unique_ptr<FtpSession> session_;
    int dataFd_ = -1;
};

} // namespace

FtpProvider::FtpProvider(std::string id, std::string name, std::string address,
                         std::string user, std::string pass)
    : id_(std::move(id)),
      name_(std::move(name)),
      address_(std::move(address)),
      user_(std::move(user)),
      pass_(std::move(pass)) {}

std::string FtpProvider::displayPrefix() const {
    return name_ + "(FTP):";
}

ProviderCapabilities FtpProvider::capabilities() const {
    ProviderCapabilities caps;
    caps.canReadRange = true;
    caps.canReadSequential = true;
    caps.canWrite = true;
    caps.canPartialWrite = true;
    caps.canCreateDirectory = true;
    caps.canDelete = true;
    caps.canRename = true;
    caps.usesUtf8Paths = true;
    caps.canInstallFromSource = true;
    return caps;
}

std::vector<FileEntry> FtpProvider::listDir(const std::string& path, std::string& errOut) {
    FtpEndpoint endpoint;
    if (!parseFtpAddress(address_, endpoint, errOut))
        return {};

    std::string remotePath = joinRemotePath(endpoint.basePath, path);
    if (!validateCommandArgument(remotePath, errOut))
        return {};

    FtpSession session(endpoint, user_, pass_);
    if (!session.open(errOut))
        return {};

    std::vector<FileEntry> entries;
    if (!session.listDirectory(remotePath, entries, errOut))
        return {};

    std::sort(entries.begin(), entries.end(), [](const FileEntry& a, const FileEntry& b) {
        if (a.isDirectory != b.isDirectory)
            return a.isDirectory > b.isDirectory;
        return a.name < b.name;
    });
    return entries;
}

bool FtpProvider::statPath(const std::string& path, FileStatInfo& out, std::string& errOut) {
    FtpEndpoint endpoint;
    if (!parseFtpAddress(address_, endpoint, errOut)) {
        out = {};
        return false;
    }

    std::string remotePath = joinRemotePath(endpoint.basePath, path);
    if (!validateCommandArgument(remotePath, errOut)) {
        out = {};
        return false;
    }

    FtpSession session(endpoint, user_, pass_);
    if (!session.open(errOut)) {
        out = {};
        return false;
    }
    return lookupRemoteEntry(session, remotePath, endpoint.basePath, out, errOut);
}

bool FtpProvider::createDirectory(const std::string& path, std::string& errOut) {
    FtpEndpoint endpoint;
    if (!parseFtpAddress(address_, endpoint, errOut))
        return false;

    std::string remotePath = joinRemotePath(endpoint.basePath, path);
    if (remotePath == endpoint.basePath) {
        errOut = "refusing to create provider root";
        return false;
    }
    if (!validateCommandArgument(remotePath, errOut))
        return false;

    FtpSession session(endpoint, user_, pass_);
    if (!session.open(errOut))
        return false;
    return session.createDirectory(remotePath, errOut);
}

bool FtpProvider::removeAll(const std::string& path, std::string& errOut) {
    FtpEndpoint endpoint;
    if (!parseFtpAddress(address_, endpoint, errOut))
        return false;

    std::string remotePath = joinRemotePath(endpoint.basePath, path);
    if (remotePath == endpoint.basePath) {
        errOut = "refusing to delete provider root";
        return false;
    }
    if (!validateCommandArgument(remotePath, errOut))
        return false;

    FtpSession session(endpoint, user_, pass_);
    if (!session.open(errOut))
        return false;
    return removeRemoteRecursive(session, remotePath, endpoint.basePath, errOut);
}

bool FtpProvider::renamePath(const std::string& from, const std::string& to,
                             std::string& errOut) {
    FtpEndpoint endpoint;
    if (!parseFtpAddress(address_, endpoint, errOut))
        return false;

    std::string remoteFrom = joinRemotePath(endpoint.basePath, from);
    std::string remoteTo = joinRemotePath(endpoint.basePath, to);
    if (!validateCommandArgument(remoteFrom, errOut) ||
        !validateCommandArgument(remoteTo, errOut)) {
        return false;
    }

    FtpSession session(endpoint, user_, pass_);
    if (!session.open(errOut))
        return false;
    return session.renamePath(remoteFrom, remoteTo, errOut);
}

bool FtpProvider::readFile(const std::string& path, uint64_t offset, size_t size,
                           void* outBuffer, std::string& errOut) {
    if (size == 0)
        return true;

    FtpEndpoint endpoint;
    if (!parseFtpAddress(address_, endpoint, errOut))
        return false;

    std::string remotePath = joinRemotePath(endpoint.basePath, path);
    if (!validateCommandArgument(remotePath, errOut))
        return false;

    FtpSession session(endpoint, user_, pass_);
    if (!session.open(errOut))
        return false;

    int dataFd = -1;
    if (!session.openReadData(remotePath, offset, dataFd, errOut))
        return false;

    auto* ptr = static_cast<unsigned char*>(outBuffer);
    size_t received = 0;
    while (received < size) {
        int rc = ::recv(dataFd, ptr + received, size - received, 0);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            errOut = std::strerror(errno);
            ::shutdown(dataFd, SHUT_RDWR);
            ::close(dataFd);
            session.close();
            return false;
        }
        if (rc == 0) {
            errOut = "unexpected eof";
            ::close(dataFd);
            session.close();
            return false;
        }
        received += static_cast<size_t>(rc);
    }

    ::shutdown(dataFd, SHUT_RDWR);
    ::close(dataFd);
    session.close();
    return true;
}

std::unique_ptr<SequentialFileReader>
FtpProvider::openSequentialRead(const std::string& path, uint64_t offset, std::string& errOut) {
    FtpEndpoint endpoint;
    if (!parseFtpAddress(address_, endpoint, errOut))
        return nullptr;

    std::string remotePath = joinRemotePath(endpoint.basePath, path);
    if (!validateCommandArgument(remotePath, errOut))
        return nullptr;

    auto reader = std::make_unique<FtpSequentialReader>(endpoint, user_, pass_, remotePath, offset);
    if (!reader->open(errOut))
        return nullptr;
    return reader;
}

bool FtpProvider::writeFile(const std::string& path, const void* data, size_t size,
                            std::string& errOut) {
    return writeFileChunk(path, 0, data, size, true, errOut);
}

bool FtpProvider::writeFileChunk(const std::string& path, uint64_t offset,
                                 const void* data, size_t size, bool truncate,
                                 std::string& errOut) {
    FtpEndpoint endpoint;
    if (!parseFtpAddress(address_, endpoint, errOut))
        return false;

    std::string remotePath = joinRemotePath(endpoint.basePath, path);
    if (!validateCommandArgument(remotePath, errOut))
        return false;

    if (!truncate && size == 0)
        return true;

    FtpSession session(endpoint, user_, pass_);
    if (!session.open(errOut))
        return false;

    int dataFd = -1;
    if (!session.openWriteData(remotePath, offset, truncate, dataFd, errOut))
        return false;

    bool ok = true;
    if (size > 0) {
        ok = sendAll(dataFd, data, size, errOut);
    }
    if (!ok) {
        ::shutdown(dataFd, SHUT_RDWR);
        ::close(dataFd);
        session.close();
        return false;
    }
    ::shutdown(dataFd, SHUT_WR);
    if (!session.finishDataTransfer(dataFd, errOut))
        ok = false;
    return ok;
}

} // namespace fs
} // namespace xxplore
