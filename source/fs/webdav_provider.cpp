#include "fs/webdav_provider.hpp"
#include <algorithm>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <curl/curl.h>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xxplore {
namespace fs {

namespace {

void debugLog(const char* fmt, ...) {
#ifdef XXPLORE_DEBUG
    std::va_list args;
    va_start(args, fmt);
    std::printf("[webdav] ");
    std::vprintf(fmt, args);
    std::printf("\n");
    va_end(args);
#else
    (void)fmt;
#endif
}

// Curl write callback: append to std::string
size_t curlWriteCb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    size_t total = size * nmemb;
    out->append(static_cast<char*>(ptr), total);
    return total;
}

// Curl read callback: read from buffer
struct ReadCtx {
    const char* data;
    size_t remaining;
};

size_t curlReadCb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<ReadCtx*>(userdata);
    size_t maxBytes = size * nmemb;
    size_t toSend = std::min(maxBytes, ctx->remaining);
    if (toSend > 0) {
        std::memcpy(ptr, ctx->data, toSend);
        ctx->data += toSend;
        ctx->remaining -= toSend;
    }
    return toSend;
}

struct ProviderReadCtx {
    FileProvider* srcProv = nullptr;
    const std::string* srcPath = nullptr;
    const ProviderProgressCb* progressCb = nullptr;
    uint64_t offset = 0;
    uint64_t remaining = 0;
    std::string error;
};

struct StreamReadCtx {
    const WebDavProvider::StreamReadFn* readFn = nullptr;
    const WebDavProvider::StreamProgressFn* progressCb = nullptr;
    uint64_t offset = 0;
    uint64_t remaining = 0;
    std::string error;
};

class WebDavSequentialReader final : public SequentialFileReader {
public:
    WebDavSequentialReader(std::string url, std::string user, std::string pass, uint64_t offset)
        : url_(std::move(url)), user_(std::move(user)), pass_(std::move(pass)), offset_(offset) {}

    ~WebDavSequentialReader() override { close(); }

    bool start(std::string& errOut) {
        close();
        stopRequested_ = false;
        finished_ = false;
        started_ = false;
        httpCode_ = 0;
        queueBytes_ = 0;
        error_.clear();
        worker_ = std::thread([this]() { workerMain(); });

        std::unique_lock<std::mutex> lock(mutex_);
        cvData_.wait(lock, [this]() {
            return stopRequested_ || !error_.empty() || !queue_.empty() || finished_ || started_;
        });
        if (!error_.empty()) {
            errOut = error_;
            lock.unlock();
            close();
            return false;
        }
        return true;
    }

    bool read(void* outBuffer, size_t size, std::string& errOut) override {
        if (size == 0)
            return true;

        auto* out = static_cast<unsigned char*>(outBuffer);
        size_t filled = 0;

        std::unique_lock<std::mutex> lock(mutex_);
        while (filled < size) {
            cvData_.wait(lock, [this]() {
                return stopRequested_ || !error_.empty() || !queue_.empty() || finished_;
            });

            if (!error_.empty()) {
                errOut = error_;
                return false;
            }

            if (queue_.empty()) {
                if (finished_) {
                    errOut = "unexpected eof";
                    return false;
                }
                continue;
            }

            Chunk& chunk = queue_.front();
            size_t avail = chunk.data.size() - chunk.offset;
            size_t take = std::min(avail, size - filled);
            std::memcpy(out + filled, chunk.data.data() + chunk.offset, take);
            chunk.offset += take;
            filled += take;
            queueBytes_ -= take;
            if (chunk.offset >= chunk.data.size())
                queue_.pop_front();
            cvSpace_.notify_one();
        }
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopRequested_ = true;
            cvData_.notify_all();
            cvSpace_.notify_all();
        }
        if (worker_.joinable())
            worker_.join();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.clear();
            queueBytes_ = 0;
        }
    }

private:
    struct Chunk {
        std::vector<unsigned char> data;
        size_t offset = 0;
    };

    static size_t curlWriteStreamingCb(void* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* self = static_cast<WebDavSequentialReader*>(userdata);
        return self ? self->onWrite(static_cast<unsigned char*>(ptr), size * nmemb) : 0;
    }

    size_t onWrite(const unsigned char* data, size_t size) {
        if (size == 0)
            return 0;

        std::unique_lock<std::mutex> lock(mutex_);
        started_ = true;
        cvData_.notify_all();

        while (!stopRequested_ && queueBytes_ >= kMaxQueueBytes)
            cvSpace_.wait(lock);
        if (stopRequested_)
            return 0;

        Chunk chunk;
        chunk.data.assign(data, data + size);
        queueBytes_ += size;
        queue_.push_back(std::move(chunk));
        cvData_.notify_one();
        return size;
    }

    void workerMain() {
        CURL* curl = curl_easy_init();
        if (!curl) {
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = "curl_easy_init failed";
            finished_ = true;
            cvData_.notify_all();
            return;
        }

        std::string range;
        if (offset_ > 0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%llu-",
                          static_cast<unsigned long long>(offset_));
            range = buf;
        }

        curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteStreamingCb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 128L * 1024L);
        if (!range.empty())
            curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());

        if (!user_.empty()) {
            curl_easy_setopt(curl, CURLOPT_USERNAME, user_.c_str());
            curl_easy_setopt(curl, CURLOPT_PASSWORD, pass_.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC | CURLAUTH_DIGEST);
        }

        CURLcode rc = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_easy_cleanup(curl);

        std::lock_guard<std::mutex> lock(mutex_);
        httpCode_ = httpCode;
        if (!stopRequested_) {
            if (rc != CURLE_OK) {
                error_ = curl_easy_strerror(rc);
            } else if (httpCode_ != 200 && httpCode_ != 206) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "GET HTTP %ld", httpCode_);
                error_ = buf;
            }
        }
        finished_ = true;
        cvData_.notify_all();
        cvSpace_.notify_all();
    }

    static constexpr size_t kMaxQueueBytes = 1024 * 1024;

    std::string url_;
    std::string user_;
    std::string pass_;
    uint64_t offset_ = 0;

    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cvData_;
    std::condition_variable cvSpace_;
    std::deque<Chunk> queue_;
    size_t queueBytes_ = 0;
    bool started_ = false;
    bool stopRequested_ = false;
    bool finished_ = false;
    long httpCode_ = 0;
    std::string error_;
};

size_t curlProviderReadCb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<ProviderReadCtx*>(userdata);
    if (!ctx || !ctx->srcProv || !ctx->srcPath)
        return CURL_READFUNC_ABORT;

    if (ctx->remaining == 0)
        return 0;

    size_t maxBytes = size * nmemb;
    size_t toRead = static_cast<size_t>(
        std::min<uint64_t>(ctx->remaining, static_cast<uint64_t>(maxBytes)));

    if (ctx->progressCb && *ctx->progressCb && !(*ctx->progressCb)(*ctx->srcPath)) {
        ctx->error = "interrupted";
        return CURL_READFUNC_ABORT;
    }

    if (!ctx->srcProv->readFile(*ctx->srcPath, ctx->offset, toRead, ptr, ctx->error))
        return CURL_READFUNC_ABORT;

    ctx->offset += toRead;
    ctx->remaining -= toRead;
    return toRead;
}

size_t curlStreamReadCb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<StreamReadCtx*>(userdata);
    if (!ctx || !ctx->readFn)
        return CURL_READFUNC_ABORT;

    if (ctx->remaining == 0)
        return 0;

    size_t maxBytes = size * nmemb;
    size_t toRead = static_cast<size_t>(
        std::min<uint64_t>(ctx->remaining, static_cast<uint64_t>(maxBytes)));
    if (toRead == 0)
        return 0;

    if (!(*ctx->readFn)(ptr, toRead, ctx->offset, ctx->error))
        return CURL_READFUNC_ABORT;

    ctx->offset += toRead;
    ctx->remaining -= toRead;

    if (ctx->progressCb && *ctx->progressCb && !(*ctx->progressCb)(ctx->offset)) {
        ctx->error = "interrupted";
        return CURL_READFUNC_ABORT;
    }

    return toRead;
}

// Minimal XML parsing helpers (no dependency on libxml)
// We only need to extract href, content-length, resource-type from PROPFIND responses

std::string xmlUnescape(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '&') {
            if (s.compare(i, 4, "&lt;") == 0)       { r += '<'; i += 3; }
            else if (s.compare(i, 4, "&gt;") == 0)   { r += '>'; i += 3; }
            else if (s.compare(i, 5, "&amp;") == 0)   { r += '&'; i += 4; }
            else if (s.compare(i, 6, "&quot;") == 0)   { r += '"'; i += 5; }
            else if (s.compare(i, 6, "&apos;") == 0)   { r += '\''; i += 5; }
            else r += s[i];
        } else {
            r += s[i];
        }
    }
    return r;
}

// URL-decode a string (from WebDAV href)
std::string urlDecode(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            unsigned val = 0;
            if (std::sscanf(s.c_str() + i + 1, "%2x", &val) == 1) {
                r += static_cast<char>(val);
                i += 2;
                continue;
            }
        }
        r += s[i];
    }
    return r;
}

// URL-encode path component (spaces, non-ASCII, special chars)
std::string urlEncodePath(const std::string& s) {
    std::string r;
    r.reserve(s.size() * 2);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            r += static_cast<char>(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            r += buf;
        }
    }
    return r;
}

struct XmlTag {
    size_t start = std::string::npos;
    size_t end = std::string::npos;
    size_t nameStart = std::string::npos;
    size_t nameEnd = std::string::npos;
    bool closing = false;
};

bool parseXmlTagAt(const std::string& xml, size_t pos, XmlTag& out) {
    if (pos >= xml.size() || xml[pos] != '<')
        return false;

    size_t p = pos + 1;
    bool closing = false;
    if (p < xml.size() && xml[p] == '/') {
        closing = true;
        p++;
    }
    if (p >= xml.size() || xml[p] == '!' || xml[p] == '?')
        return false;

    size_t nameStart = p;
    while (p < xml.size()) {
        char c = xml[p];
        if (c == '>' || c == '/' || c == ' ' || c == '\t' || c == '\r' || c == '\n')
            break;
        p++;
    }
    if (p == nameStart)
        return false;

    size_t gt = xml.find('>', p);
    if (gt == std::string::npos)
        return false;

    out.start = pos;
    out.end = gt + 1;
    out.nameStart = nameStart;
    out.nameEnd = p;
    out.closing = closing;
    return true;
}

std::string xmlLocalName(const std::string& xml, const XmlTag& tag) {
    if (tag.nameStart == std::string::npos || tag.nameEnd == std::string::npos ||
        tag.nameEnd <= tag.nameStart) {
        return {};
    }

    std::string name = xml.substr(tag.nameStart, tag.nameEnd - tag.nameStart);
    size_t colonPos = name.rfind(':');
    return colonPos == std::string::npos ? name : name.substr(colonPos + 1);
}

bool findNextXmlTag(const std::string& xml, size_t from, size_t limit,
                    const char* localName, bool closing, XmlTag& out) {
    size_t pos = from;
    while (pos < limit) {
        size_t lt = xml.find('<', pos);
        if (lt == std::string::npos || lt >= limit)
            return false;

        XmlTag tag;
        if (parseXmlTagAt(xml, lt, tag) && tag.end <= limit &&
            tag.closing == closing && xmlLocalName(xml, tag) == localName) {
            out = tag;
            return true;
        }
        pos = lt + 1;
    }
    return false;
}

std::string extractXmlElementText(const std::string& xml, size_t start, size_t end,
                                  const char* localName) {
    XmlTag openTag;
    if (!findNextXmlTag(xml, start, end, localName, false, openTag))
        return {};

    XmlTag closeTag;
    if (!findNextXmlTag(xml, openTag.end, end, localName, true, closeTag))
        return {};

    if (closeTag.start < openTag.end)
        return {};

    return xml.substr(openTag.end, closeTag.start - openTag.end);
}

bool containsXmlElement(const std::string& xml, size_t start, size_t end, const char* localName) {
    XmlTag tag;
    return findNextXmlTag(xml, start, end, localName, false, tag);
}

std::string hrefPathOnly(const std::string& href) {
    size_t scheme = href.find("://");
    if (scheme == std::string::npos)
        return href;

    size_t pathStart = href.find('/', scheme + 3);
    if (pathStart == std::string::npos)
        return "/";
    return href.substr(pathStart);
}

std::string normalizeDavPath(std::string path) {
    if (path.empty())
        path = "/";
    size_t queryPos = path.find_first_of("?#");
    if (queryPos != std::string::npos)
        path.erase(queryPos);
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    return path;
}

} // namespace

WebDavProvider::WebDavProvider(std::string id, std::string name, std::string url,
                               std::string user, std::string pass)
    : id_(std::move(id)), name_(std::move(name)), baseUrl_(std::move(url)),
      user_(std::move(user)), pass_(std::move(pass)) {
    // Normalize: remove trailing slash
    while (!baseUrl_.empty() && baseUrl_.back() == '/')
        baseUrl_.pop_back();
}

WebDavProvider::~WebDavProvider() = default;

std::string WebDavProvider::displayPrefix() const {
    return name_ + "(WebDAV):";
}

std::string WebDavProvider::makeUrl(const std::string& relPath) const {
    if (relPath.empty() || relPath == "/")
        return baseUrl_ + "/";
    std::string path = relPath;
    if (path.front() != '/')
        path = "/" + path;
    return baseUrl_ + urlEncodePath(path);
}

// --- Curl operations ---

WebDavProvider::CurlResult WebDavProvider::performPropfind(const std::string& url, int depth) {
    CurlResult result;
    debugLog("PROPFIND url=%s depth=%d", url.c_str(), depth);
    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error = "curl_easy_init failed";
        return result;
    }

    std::string depthStr = std::to_string(depth);
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Depth: " + depthStr).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/xml");

    // Minimal PROPFIND body requesting only essential properties
    const char* body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<D:propfind xmlns:D=\"DAV:\">"
        "<D:prop>"
        "<D:resourcetype/>"
        "<D:getcontentlength/>"
        "<D:getlastmodified/>"
        "</D:prop>"
        "</D:propfind>";

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PROPFIND");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    if (!user_.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERNAME, user_.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, pass_.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC | CURLAUTH_DIGEST);
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        result.error = curl_easy_strerror(rc);
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.httpCode);
    }
    debugLog("PROPFIND done url=%s http=%ld err=%s body=%zu", url.c_str(), result.httpCode,
             result.error.empty() ? "-" : result.error.c_str(), result.body.size());

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
}

WebDavProvider::CurlResult WebDavProvider::performRequest(const std::string& method,
                                                          const std::string& url,
                                                          const void* sendData,
                                                          size_t sendSize) {
    CurlResult result;
    debugLog("%s url=%s send=%zu", method.c_str(), url.c_str(), sendSize);
    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error = "curl_easy_init failed";
        return result;
    }

    ReadCtx readCtx{static_cast<const char*>(sendData), sendSize};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    if (sendData && sendSize > 0) {
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, curlReadCb);
        curl_easy_setopt(curl, CURLOPT_READDATA, &readCtx);
        curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(sendSize));
    }

    if (!user_.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERNAME, user_.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, pass_.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC | CURLAUTH_DIGEST);
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        result.error = curl_easy_strerror(rc);
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.httpCode);
    }
    debugLog("%s done url=%s http=%ld err=%s body=%zu", method.c_str(), url.c_str(),
             result.httpCode, result.error.empty() ? "-" : result.error.c_str(),
             result.body.size());

    curl_easy_cleanup(curl);
    return result;
}

WebDavProvider::CurlResult WebDavProvider::performGet(const std::string& url,
                                                      uint64_t offset, size_t size) {
    CurlResult result;
    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error = "curl_easy_init failed";
        return result;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    // Range header for partial reads
    if (size > 0) {
        char range[128];
        std::snprintf(range, sizeof(range), "%llu-%llu",
                      static_cast<unsigned long long>(offset),
                      static_cast<unsigned long long>(offset + size - 1));
        curl_easy_setopt(curl, CURLOPT_RANGE, range);
    }

    if (!user_.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERNAME, user_.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, pass_.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC | CURLAUTH_DIGEST);
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        result.error = curl_easy_strerror(rc);
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.httpCode);
    }
    if (!result.error.empty() || (result.httpCode != 206 && result.httpCode != 200)) {
        debugLog("GET failed url=%s offset=%llu size=%zu http=%ld err=%s body=%zu",
                 url.c_str(), static_cast<unsigned long long>(offset), size, result.httpCode,
                 result.error.empty() ? "-" : result.error.c_str(), result.body.size());
    }

    curl_easy_cleanup(curl);
    return result;
}

WebDavProvider::CurlResult WebDavProvider::performStreamPut(const std::string& url,
                                                            FileProvider* srcProv,
                                                            const std::string& srcPath,
                                                            uint64_t size,
                                                            const ProviderProgressCb& cb) {
    CurlResult result;
    debugLog("PUT(stream) url=%s size=%llu", url.c_str(),
             static_cast<unsigned long long>(size));

    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error = "curl_easy_init failed";
        return result;
    }

    ProviderReadCtx readCtx;
    readCtx.srcProv = srcProv;
    readCtx.srcPath = &srcPath;
    readCtx.progressCb = &cb;
    readCtx.offset = 0;
    readCtx.remaining = size;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, curlProviderReadCb);
    curl_easy_setopt(curl, CURLOPT_READDATA, &readCtx);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(size));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    if (!user_.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERNAME, user_.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, pass_.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC | CURLAUTH_DIGEST);
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        result.error = !readCtx.error.empty() ? readCtx.error : curl_easy_strerror(rc);
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.httpCode);
    }
    debugLog("PUT(stream) done url=%s http=%ld err=%s body=%zu", url.c_str(),
             result.httpCode, result.error.empty() ? "-" : result.error.c_str(),
             result.body.size());

    curl_easy_cleanup(curl);
    return result;
}

WebDavProvider::CurlResult WebDavProvider::performStreamPut(const std::string& url,
                                                            uint64_t size,
                                                            const StreamReadFn& readFn,
                                                            const StreamProgressFn& progressCb) {
    CurlResult result;
    debugLog("PUT(stream-cb) url=%s size=%llu", url.c_str(),
             static_cast<unsigned long long>(size));

    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error = "curl_easy_init failed";
        return result;
    }

    StreamReadCtx readCtx;
    readCtx.readFn = &readFn;
    readCtx.progressCb = &progressCb;
    readCtx.offset = 0;
    readCtx.remaining = size;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, curlStreamReadCb);
    curl_easy_setopt(curl, CURLOPT_READDATA, &readCtx);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(size));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    if (!user_.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERNAME, user_.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, pass_.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC | CURLAUTH_DIGEST);
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        result.error = !readCtx.error.empty() ? readCtx.error : curl_easy_strerror(rc);
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.httpCode);
    }
    debugLog("PUT(stream-cb) done url=%s http=%ld err=%s body=%zu", url.c_str(),
             result.httpCode, result.error.empty() ? "-" : result.error.c_str(),
             result.body.size());

    curl_easy_cleanup(curl);
    return result;
}

// --- PROPFIND XML parsing ---

std::vector<FileEntry> WebDavProvider::parsePropfindResponse(const std::string& xml,
                                                             const std::string& basePath) {
    std::vector<FileEntry> entries;
    std::string normalizedBase = normalizeDavPath(basePath);

    size_t pos = 0;
    while (pos < xml.size()) {
        XmlTag responseOpen;
        if (!findNextXmlTag(xml, pos, xml.size(), "response", false, responseOpen))
            break;

        XmlTag responseClose;
        if (!findNextXmlTag(xml, responseOpen.end, xml.size(), "response", true, responseClose))
            break;
        size_t blockStart = responseOpen.start;
        size_t blockEnd = responseClose.end;

        std::string href = extractXmlElementText(xml, blockStart, blockEnd, "href");

        if (href.empty()) {
            pos = blockEnd;
            continue;
        }

        href = xmlUnescape(href);
        href = urlDecode(href);
        std::string hrefPath = normalizeDavPath(hrefPathOnly(href));

        // Extract name from href
        auto lastSlash = hrefPath.rfind('/');
        std::string name = (lastSlash != std::string::npos)
                               ? hrefPath.substr(lastSlash + 1)
                               : hrefPath;

        // Check if collection
        bool isDir = containsXmlElement(xml, blockStart, blockEnd, "collection");

        // Get content length
        uint64_t fileSize = 0;
        std::string sizeStr = extractXmlElementText(xml, blockStart, blockEnd, "getcontentlength");
        if (!sizeStr.empty())
            fileSize = std::strtoull(sizeStr.c_str(), nullptr, 10);

        bool isSelf = name.empty() || hrefPath == normalizedBase;

        if (!isSelf && !name.empty()) {
            FileEntry e;
            e.name = name;
            e.isDirectory = isDir;
            e.size = fileSize;
            debugLog("parsed entry href=%s path=%s name=%s dir=%d size=%llu", href.c_str(),
                     hrefPath.c_str(), name.c_str(), isDir ? 1 : 0,
                     static_cast<unsigned long long>(fileSize));
            entries.push_back(std::move(e));
        } else {
            debugLog("skip entry href=%s path=%s self=%d name=%s", href.c_str(), hrefPath.c_str(),
                     isSelf ? 1 : 0, name.c_str());
        }

        pos = blockEnd;
    }

    // Sort: directories first, then by name
    std::sort(entries.begin(), entries.end(), [](const FileEntry& a, const FileEntry& b) {
        if (a.isDirectory != b.isDirectory) return a.isDirectory > b.isDirectory;
        return a.name < b.name;
    });

    return entries;
}

// --- Public FileProvider API ---

bool WebDavProvider::testConnection(std::string& errOut) {
    auto res = performPropfind(baseUrl_ + "/", 0);
    if (!res.error.empty()) {
        errOut = res.error;
        return false;
    }
    if (res.httpCode < 200 || res.httpCode >= 300) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "HTTP %ld", res.httpCode);
        errOut = buf;
        return false;
    }
    return true;
}

std::vector<FileEntry> WebDavProvider::listDir(const std::string& path, std::string& errOut) {
    std::string url = makeUrl(path);
    if (url.back() != '/') url += '/';
    debugLog("listDir rel=%s url=%s", path.c_str(), url.c_str());

    auto res = performPropfind(url, 1);
    if (!res.error.empty()) {
        errOut = res.error;
        debugLog("listDir failed rel=%s err=%s", path.c_str(), errOut.c_str());
        return {};
    }
    if (res.httpCode != 207 && res.httpCode != 200) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "HTTP %ld", res.httpCode);
        errOut = buf;
        debugLog("listDir failed rel=%s err=%s", path.c_str(), errOut.c_str());
        return {};
    }

    // Extract the path component from the URL for comparison
    // Find the path after host
    std::string urlPath;
    auto schemeEnd = url.find("://");
    if (schemeEnd != std::string::npos) {
        auto pathStart = url.find('/', schemeEnd + 3);
        if (pathStart != std::string::npos)
            urlPath = url.substr(pathStart);
    }
    if (urlPath.empty()) urlPath = "/";
    urlPath = urlDecode(urlPath);

    auto entries = parsePropfindResponse(res.body, urlPath);
    debugLog("listDir done rel=%s count=%zu", path.c_str(), entries.size());
    return entries;
}

bool WebDavProvider::statPath(const std::string& path, FileStatInfo& out, std::string& errOut) {
    std::string url = makeUrl(path);
    debugLog("stat rel=%s url=%s", path.c_str(), url.c_str());
    auto res = performPropfind(url, 0);
    if (!res.error.empty()) {
        errOut = res.error;
        debugLog("stat failed rel=%s err=%s", path.c_str(), errOut.c_str());
        return false;
    }
    if (res.httpCode == 404) {
        out.exists = false;
        return true;
    }
    if (res.httpCode != 207 && res.httpCode != 200) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "HTTP %ld", res.httpCode);
        errOut = buf;
        debugLog("stat failed rel=%s err=%s", path.c_str(), errOut.c_str());
        return false;
    }

    out.exists = true;
    out.isDirectory = false;
    out.size = 0;

    // Check for collection
    for (const char* tag : {"<D:collection", "<d:collection"}) {
        if (res.body.find(tag) != std::string::npos) {
            out.isDirectory = true;
            break;
        }
    }

    // Get content length
    for (const char* open : {"<D:getcontentlength>", "<d:getcontentlength>"}) {
        auto p = res.body.find(open);
        if (p != std::string::npos) {
            p += std::strlen(open);
            out.size = std::strtoull(res.body.c_str() + p, nullptr, 10);
            break;
        }
    }

    return true;
}

bool WebDavProvider::createDirectory(const std::string& path, std::string& errOut) {
    std::string url = makeUrl(path);
    if (url.back() != '/') url += '/';

    auto res = performRequest("MKCOL", url);
    if (!res.error.empty()) {
        errOut = res.error;
        return false;
    }
    if (res.httpCode != 201 && res.httpCode != 200) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "MKCOL HTTP %ld", res.httpCode);
        errOut = buf;
        return false;
    }
    return true;
}

bool WebDavProvider::removeAll(const std::string& path, std::string& errOut) {
    std::string url = makeUrl(path);

    auto res = performRequest("DELETE", url);
    if (!res.error.empty()) {
        errOut = res.error;
        return false;
    }
    if (res.httpCode != 200 && res.httpCode != 204 && res.httpCode != 404) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "DELETE HTTP %ld", res.httpCode);
        errOut = buf;
        return false;
    }
    return true;
}

bool WebDavProvider::renamePath(const std::string& from, const std::string& to,
                                std::string& errOut) {
    std::string srcUrl = makeUrl(from);
    std::string dstUrl = makeUrl(to);

    CurlResult result;
    CURL* curl = curl_easy_init();
    if (!curl) {
        errOut = "curl_easy_init failed";
        return false;
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Destination: " + dstUrl).c_str());
    headers = curl_slist_append(headers, "Overwrite: F");

    curl_easy_setopt(curl, CURLOPT_URL, srcUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "MOVE");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    if (!user_.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERNAME, user_.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, pass_.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC | CURLAUTH_DIGEST);
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        errOut = curl_easy_strerror(rc);
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.httpCode);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (!errOut.empty())
        return false;
    if (result.httpCode != 201 && result.httpCode != 200 && result.httpCode != 204) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "MOVE HTTP %ld", result.httpCode);
        errOut = buf;
        return false;
    }
    return true;
}

bool WebDavProvider::readFile(const std::string& path, uint64_t offset, size_t size,
                              void* outBuffer, std::string& errOut) {
    std::string url = makeUrl(path);
    auto res = performGet(url, offset, size);
    if (!res.error.empty()) {
        errOut = res.error;
        return false;
    }
    if (res.httpCode != 200 && res.httpCode != 206) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "GET HTTP %ld", res.httpCode);
        errOut = buf;
        return false;
    }
    if (res.body.size() < size) {
        // Partial response is fine for range reads, copy what we have
        std::memcpy(outBuffer, res.body.data(), res.body.size());
        // Zero remaining
        if (res.body.size() < size)
            std::memset(static_cast<char*>(outBuffer) + res.body.size(), 0, size - res.body.size());
    } else {
        std::memcpy(outBuffer, res.body.data(), size);
    }
    return true;
}

std::unique_ptr<SequentialFileReader>
WebDavProvider::openSequentialRead(const std::string& path, uint64_t offset,
                                   std::string& errOut) {
    auto reader = std::make_unique<WebDavSequentialReader>(makeUrl(path), user_, pass_, offset);
    if (!reader->start(errOut))
        return nullptr;
    return reader;
}

bool WebDavProvider::writeFile(const std::string& path, const void* data, size_t size,
                               std::string& errOut) {
    std::string url = makeUrl(path);
    auto res = performRequest("PUT", url, data, size);
    if (!res.error.empty()) {
        errOut = res.error;
        return false;
    }
    if (res.httpCode != 200 && res.httpCode != 201 && res.httpCode != 204) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "PUT HTTP %ld", res.httpCode);
        errOut = buf;
        return false;
    }
    return true;
}

bool WebDavProvider::uploadFromProvider(FileProvider* srcProv, const std::string& srcPath,
                                        const std::string& dstPath, uint64_t size,
                                        std::string& errOut,
                                        const ProviderProgressCb& cb) {
    if (!srcProv) {
        errOut = "missing source provider";
        return false;
    }

    std::string url = makeUrl(dstPath);
    auto res = performStreamPut(url, srcProv, srcPath, size, cb);
    if (!res.error.empty()) {
        errOut = res.error;
        return false;
    }
    if (res.httpCode != 200 && res.httpCode != 201 && res.httpCode != 204) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "PUT HTTP %ld", res.httpCode);
        errOut = buf;
        return false;
    }
    return true;
}

bool WebDavProvider::uploadFromStream(const std::string& dstPath, uint64_t size,
                                      const StreamReadFn& readFn, std::string& errOut,
                                      const StreamProgressFn& progressCb) {
    if (!readFn) {
        errOut = "missing stream reader";
        return false;
    }

    std::string url = makeUrl(dstPath);
    auto res = performStreamPut(url, size, readFn, progressCb);
    if (!res.error.empty()) {
        errOut = res.error;
        return false;
    }
    if (res.httpCode != 200 && res.httpCode != 201 && res.httpCode != 204) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "PUT HTTP %ld", res.httpCode);
        errOut = buf;
        return false;
    }
    return true;
}

} // namespace fs
} // namespace xxplore
