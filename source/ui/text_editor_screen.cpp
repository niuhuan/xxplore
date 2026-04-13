#include "ui/text_editor_screen.hpp"

#include "fs/file_provider.hpp"
#include "fs/provider_manager.hpp"
#include "fs/webdav_provider.hpp"
#include "i18n/i18n.hpp"
#include "ui/font_manager.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "ui/touch_event.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <switch.h>
#include <utility>

namespace xxplore {

namespace {

constexpr int kHeaderHitW = 42;
constexpr int kHeaderHitH = 36;
constexpr int kHeaderIconSize = 24;
constexpr int kHeaderEdgePadding = 8;
constexpr int kHeaderPathTop = 28;

constexpr int kRowHeight = 34;
constexpr int kTextFontSize = 18;
constexpr int kLineNumberFontSize = 16;
constexpr int kContentPadX = 12;
constexpr int kContentPadY = 8;
constexpr int kLineNumberLeftPad = 6;
constexpr int kLineNumberRightGap = 12;
constexpr int kTextLeftPad = 14;
constexpr int kSelectionBarW = 3;

constexpr std::size_t kScanChunkSize = 64U * 1024U;
constexpr std::size_t kPreviewByteLimit = 1024U;
constexpr std::size_t kRewriteChunkSize = 128U * 1024U;

constexpr int kClipboardBodyFontSize = 16;

struct TouchButtonsLayout {
    bool enabled = false;
    int prevX = 0;
    int nextX = 0;
    int pageW = 0;
    int selectX = 0;
    int selectW = 0;
    int y = 0;
    int h = 0;
    int contentLeft = 0;
    int contentRight = 0;
};

int visibleRowCount() {
    const int contentH = theme::SCREEN_H - theme::HEADER_H - theme::FOOTER_H - kContentPadY * 2;
    return std::max(1, contentH / kRowHeight);
}

int contentTop() {
    return theme::HEADER_H + kContentPadY;
}

int contentHeight() {
    return theme::SCREEN_H - theme::HEADER_H - theme::FOOTER_H - kContentPadY * 2;
}

int footerY() {
    return theme::SCREEN_H - theme::FOOTER_H;
}

TouchButtonsLayout touchButtonsLayout(bool enabled) {
    TouchButtonsLayout layout;
    if (!enabled) {
        layout.contentLeft = theme::PADDING;
        layout.contentRight = theme::SCREEN_W - theme::PADDING;
        return layout;
    }

    constexpr int kButtonGap = 12;
    constexpr int kSidePadding = 14;
    layout.enabled = true;
    layout.pageW = 42;
    layout.selectW = 42;
    layout.h = 28;
    layout.y = theme::HEADER_H + theme::PANEL_CONTENT_H + (theme::FOOTER_H - layout.h) / 2;
    layout.prevX = kSidePadding;
    layout.nextX = layout.prevX + layout.pageW + kButtonGap;
    layout.selectX = theme::SCREEN_W - kSidePadding - layout.selectW;
    layout.contentLeft = layout.nextX + layout.pageW + 20;
    layout.contentRight = layout.selectX - 20;
    return layout;
}

void drawThickChevron(Renderer& renderer, int x, int y, int w, int h, bool left,
                      SDL_Color color) {
    int cx = x + w / 2;
    int cy = y + h / 2;
    int halfH = 7;
    int halfW = 4;
    int dir = left ? -1 : 1;
    for (int offset = -1; offset <= 1; ++offset) {
        renderer.drawLine(cx + dir * halfW, cy - halfH + offset, cx - dir * halfW, cy + offset,
                          color);
        renderer.drawLine(cx - dir * halfW, cy + offset, cx + dir * halfW, cy + halfH + offset,
                          color);
    }
}

void drawSelectIcon(Renderer& renderer, int x, int y, int w, int h, SDL_Color color) {
    const int boxW = 11;
    const int boxH = 11;
    const int boxX = x + (w - boxW) / 2;
    const int boxY = y + (h - boxH) / 2;
    renderer.drawRect(boxX, boxY, boxW, boxH, color);
    renderer.drawLine(boxX + 2, boxY + 6, boxX + 5, boxY + 9, color);
    renderer.drawLine(boxX + 5, boxY + 9, boxX + 10, boxY + 3, color);
    renderer.drawLine(boxX + 2, boxY + 7, boxX + 5, boxY + 10, color);
    renderer.drawLine(boxX + 5, boxY + 10, boxX + 10, boxY + 4, color);
}

std::string zeroPaddedLineNumber(uint64_t lineNumber) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04llu",
                  static_cast<unsigned long long>(lineNumber));
    return buffer;
}

bool isValidUtf8Sequence(const std::string& value, std::size_t index, std::size_t& advance) {
    advance = 1;
    const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
    const unsigned char lead = bytes[index];
    const std::size_t remaining = value.size() - index;

    if (lead < 0x80) {
        advance = 1;
        return true;
    }

    auto cont = [&](std::size_t at) {
        return at < value.size() && (bytes[at] & 0xC0) == 0x80;
    };

    if (lead >= 0xC2 && lead <= 0xDF) {
        if (remaining < 2 || !cont(index + 1))
            return false;
        advance = 2;
        return true;
    }

    if (lead == 0xE0) {
        if (remaining < 3 || !cont(index + 1) || !cont(index + 2))
            return false;
        if (bytes[index + 1] < 0xA0)
            return false;
        advance = 3;
        return true;
    }
    if (lead >= 0xE1 && lead <= 0xEC) {
        if (remaining < 3 || !cont(index + 1) || !cont(index + 2))
            return false;
        advance = 3;
        return true;
    }
    if (lead == 0xED) {
        if (remaining < 3 || !cont(index + 1) || !cont(index + 2))
            return false;
        if (bytes[index + 1] >= 0xA0)
            return false;
        advance = 3;
        return true;
    }
    if (lead >= 0xEE && lead <= 0xEF) {
        if (remaining < 3 || !cont(index + 1) || !cont(index + 2))
            return false;
        advance = 3;
        return true;
    }

    if (lead == 0xF0) {
        if (remaining < 4 || !cont(index + 1) || !cont(index + 2) || !cont(index + 3))
            return false;
        if (bytes[index + 1] < 0x90)
            return false;
        advance = 4;
        return true;
    }
    if (lead >= 0xF1 && lead <= 0xF3) {
        if (remaining < 4 || !cont(index + 1) || !cont(index + 2) || !cont(index + 3))
            return false;
        advance = 4;
        return true;
    }
    if (lead == 0xF4) {
        if (remaining < 4 || !cont(index + 1) || !cont(index + 2) || !cont(index + 3))
            return false;
        if (bytes[index + 1] > 0x8F)
            return false;
        advance = 4;
        return true;
    }

    return false;
}

std::string sanitizeUtf8ForDisplay(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());

    for (std::size_t i = 0; i < raw.size();) {
        std::size_t advance = 1;
        if (isValidUtf8Sequence(raw, i, advance)) {
            out.append(raw, i, advance);
            i += advance;
            continue;
        }

        unsigned char ch = static_cast<unsigned char>(raw[i]);
        if (ch == '\t') {
            out.append("    ");
        } else {
            out.push_back('?');
        }
        ++i;
    }

    return out;
}

std::string parentDir(const std::string& path) {
    std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return path;
    return path.substr(0, slash + 1);
}

std::string baseName(const std::string& path) {
    std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return path;
    if (slash + 1 >= path.size())
        return {};
    return path.substr(slash + 1);
}

class RewriteStream {
public:
    RewriteStream(fs::ProviderManager& provMgr, std::string sourcePath,
                  const std::vector<TextEditorScreen::RewriteSegment>& segments)
        : provMgr_(provMgr), sourcePath_(std::move(sourcePath)), segments_(segments) {
        for (const auto& segment : segments_)
            totalSize_ += segment.length;
    }

    uint64_t totalSize() const { return totalSize_; }

    bool read(void* outBuffer, size_t size, std::string& errOut) {
        auto* out = static_cast<unsigned char*>(outBuffer);
        size_t filled = 0;

        while (filled < size) {
            if (segmentIndex_ >= segments_.size()) {
                errOut = "rewrite stream ended unexpectedly";
                return false;
            }

            const auto& segment = segments_[segmentIndex_];
            const uint64_t remainingInSegment = segment.length - segmentOffset_;
            if (remainingInSegment == 0) {
                ++segmentIndex_;
                segmentOffset_ = 0;
                continue;
            }

            const size_t chunk = static_cast<size_t>(
                std::min<uint64_t>(remainingInSegment, static_cast<uint64_t>(size - filled)));
            if (segment.kind == TextEditorScreen::RewriteSegment::Kind::SourceRange) {
                if (!provMgr_.readFile(sourcePath_, segment.sourceOffset + segmentOffset_, chunk,
                                       out + filled, errOut)) {
                    return false;
                }
            } else {
                std::memcpy(out + filled, segment.literal.data() + segmentOffset_, chunk);
            }

            filled += chunk;
            segmentOffset_ += chunk;
        }

        return true;
    }

private:
    fs::ProviderManager& provMgr_;
    std::string sourcePath_;
    const std::vector<TextEditorScreen::RewriteSegment>& segments_;
    uint64_t totalSize_ = 0;
    std::size_t segmentIndex_ = 0;
    uint64_t segmentOffset_ = 0;
};

void drawFooterTip(Renderer& renderer, FontManager& fm, int& x, int y,
                   const char* button, const char* label) {
    fm.drawText(renderer.sdl(), button, x, y, theme::FONT_SIZE_FOOTER, theme::PRIMARY);
    x += fm.measureText(button, theme::FONT_SIZE_FOOTER);
    fm.drawText(renderer.sdl(), ":", x, y, theme::FONT_SIZE_FOOTER, theme::TEXT_SECONDARY);
    x += fm.measureText(":", theme::FONT_SIZE_FOOTER);
    fm.drawText(renderer.sdl(), label, x, y, theme::FONT_SIZE_FOOTER, theme::TEXT_SECONDARY);
    x += fm.measureText(label, theme::FONT_SIZE_FOOTER);
}

} // namespace

bool TextEditorScreen::open(fs::ProviderManager& provMgr, const std::string& path,
                            std::string& errOut) {
    close();

    provMgr_ = &provMgr;
    path_ = path;
    displayPath_ = path;

    std::string relPath;
    fs::FileProvider* provider = provMgr_->resolveProvider(path_, relPath);
    if (!provider) {
        errOut = "missing provider";
        close();
        return false;
    }
    readOnly_ = provider->isReadOnly();

    if (!refreshFileStat(errOut) || !detectBom(errOut)) {
        close();
        return false;
    }

    textClipboard_.clear();
    selection_.clear();
    pageHistory_.clear();
    pendingError_.clear();

    active_ = true;
    if (!loadPage(contentStartByte_, 0, 0, true, errOut)) {
        close();
        return false;
    }
    return true;
}

void TextEditorScreen::close() {
    active_ = false;
    provMgr_ = nullptr;
    path_.clear();
    displayPath_.clear();
    readOnly_ = false;
    fileSize_ = 0;
    contentStartByte_ = 0;
    fileEndsWithNewline_ = false;
    pageLines_.clear();
    pageHistory_.clear();
    cursorIndex_ = 0;
    pageStartByte_ = 0;
    pageStartLineNumber_ = 0;
    selection_.clear();
    textClipboard_.clear();
    pendingError_.clear();
}

bool TextEditorScreen::takePendingError(std::string& errOut) {
    if (pendingError_.empty())
        return false;
    errOut = std::move(pendingError_);
    pendingError_.clear();
    return true;
}

bool TextEditorScreen::refreshFileStat(std::string& errOut) {
    if (!provMgr_) {
        errOut = "missing provider manager";
        return false;
    }

    fs::FileStatInfo statInfo;
    if (!provMgr_->statPath(path_, statInfo, errOut))
        return false;
    if (statInfo.isDirectory) {
        errOut = "path is a directory";
        return false;
    }

    fileSize_ = statInfo.size;
    if (fileSize_ <= contentStartByte_) {
        fileEndsWithNewline_ = false;
        return true;
    }

    unsigned char tail[1] = {0};
    if (!provMgr_->readFile(path_, fileSize_ - 1, 1, tail, errOut))
        return false;
    fileEndsWithNewline_ = (tail[0] == '\n' || tail[0] == '\r');
    return true;
}

bool TextEditorScreen::detectBom(std::string& errOut) {
    contentStartByte_ = 0;
    if (!provMgr_ || fileSize_ == 0)
        return true;

    const std::size_t probeSize = static_cast<std::size_t>(std::min<uint64_t>(fileSize_, 4));
    std::array<unsigned char, 4> bom = {0, 0, 0, 0};
    if (!provMgr_->readFile(path_, 0, probeSize, bom.data(), errOut))
        return false;

    if (probeSize >= 3 && bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF) {
        contentStartByte_ = 3;
        return true;
    }
    if (probeSize >= 4 && bom[0] == 0x00 && bom[1] == 0x00 && bom[2] == 0xFE && bom[3] == 0xFF) {
        errOut = "not_utf8_bom";
        return false;
    }
    if (probeSize >= 4 && bom[0] == 0xFF && bom[1] == 0xFE && bom[2] == 0x00 && bom[3] == 0x00) {
        errOut = "not_utf8_bom";
        return false;
    }
    if (probeSize >= 2 && bom[0] == 0xFE && bom[1] == 0xFF) {
        errOut = "not_utf8_bom";
        return false;
    }
    if (probeSize >= 2 && bom[0] == 0xFF && bom[1] == 0xFE) {
        errOut = "not_utf8_bom";
        return false;
    }
    return true;
}

bool TextEditorScreen::scanLine(uint64_t startByte, uint64_t lineNumber, LineView& outLine,
                                std::string& errOut) const {
    outLine = {};
    outLine.lineNumber = lineNumber;
    outLine.lineStartByte = startByte;

    if (!provMgr_) {
        errOut = "missing provider manager";
        return false;
    }
    if (startByte < contentStartByte_) {
        errOut = "invalid line start";
        return false;
    }

    if (fileSize_ <= contentStartByte_) {
        outLine.lineStartByte = contentStartByte_;
        return true;
    }

    if (startByte > fileSize_) {
        errOut = "line start out of range";
        return false;
    }

    if (startByte == fileSize_) {
        if (!fileEndsWithNewline_) {
            errOut = "line start at eof";
            return false;
        }
        return true;
    }

    uint64_t offset = startByte;
    std::string previewRaw;
    previewRaw.reserve(128);

    while (offset < fileSize_) {
        const std::size_t chunkSize = static_cast<std::size_t>(
            std::min<uint64_t>(fileSize_ - offset, static_cast<uint64_t>(kScanChunkSize)));
        std::vector<unsigned char> chunk(chunkSize);
        if (!provMgr_->readFile(path_, offset, chunkSize, chunk.data(), errOut))
            return false;

        std::size_t newlinePos = chunkSize;
        for (std::size_t i = 0; i < chunkSize; ++i) {
            if (chunk[i] == '\n' || chunk[i] == '\r') {
                newlinePos = i;
                break;
            }
        }

        const std::size_t textBytes = newlinePos;
        outLine.lineByteLength += textBytes;
        if (previewRaw.size() < kPreviewByteLimit) {
            const std::size_t previewCopy =
                std::min<std::size_t>(textBytes, kPreviewByteLimit - previewRaw.size());
            previewRaw.append(reinterpret_cast<const char*>(chunk.data()), previewCopy);
        }

        if (newlinePos == chunkSize) {
            offset += chunkSize;
            continue;
        }

        outLine.newlineByteLength = 1;
        if (chunk[newlinePos] == '\r') {
            if (newlinePos + 1 < chunkSize) {
                if (chunk[newlinePos + 1] == '\n')
                    outLine.newlineByteLength = 2;
            } else if (offset + newlinePos + 1 < fileSize_) {
                unsigned char nextByte = 0;
                if (!provMgr_->readFile(path_, offset + newlinePos + 1, 1, &nextByte, errOut))
                    return false;
                if (nextByte == '\n')
                    outLine.newlineByteLength = 2;
            }
        }
        break;
    }

    outLine.previewText = sanitizeUtf8ForDisplay(previewRaw);
    return true;
}

bool TextEditorScreen::scanPage(uint64_t startByte, uint64_t startLineNumber,
                                std::vector<LineView>& outLines, std::string& errOut) const {
    outLines.clear();

    const int rows = visibleRowCount();
    if (fileSize_ <= contentStartByte_) {
        LineView line;
        line.lineNumber = 0;
        line.lineStartByte = contentStartByte_;
        outLines.push_back(std::move(line));
        return true;
    }

    if (startByte == fileSize_ && !fileEndsWithNewline_) {
        errOut = "page start at eof";
        return false;
    }

    uint64_t nextByte = startByte;
    uint64_t nextLine = startLineNumber;
    for (int i = 0; i < rows; ++i) {
        LineView line;
        if (!scanLine(nextByte, nextLine, line, errOut))
            return false;
        outLines.push_back(line);

        const uint64_t afterLine = line.endByteExclusive();
        if (afterLine >= fileSize_ && line.newlineByteLength == 0)
            break;

        nextByte = afterLine;
        ++nextLine;

        if (nextByte == fileSize_ && !fileEndsWithNewline_)
            break;
    }

    if (outLines.empty()) {
        LineView line;
        line.lineNumber = startLineNumber;
        line.lineStartByte = startByte;
        outLines.push_back(std::move(line));
    }
    return true;
}

bool TextEditorScreen::loadPage(uint64_t startByte, uint64_t startLineNumber, int cursorIndex,
                                bool resetHistory, std::string& errOut) {
    std::vector<LineView> newLines;
    if (!scanPage(startByte, startLineNumber, newLines, errOut))
        return false;

    pageStartByte_ = startByte;
    pageStartLineNumber_ = startLineNumber;
    pageLines_ = std::move(newLines);
    if (resetHistory)
        pageHistory_.clear();
    if (pageLines_.empty()) {
        cursorIndex_ = 0;
    } else if (cursorIndex < 0) {
        cursorIndex_ = 0;
    } else {
        cursorIndex_ = std::min<int>(cursorIndex, static_cast<int>(pageLines_.size()) - 1);
    }
    return true;
}

bool TextEditorScreen::findPreviousLineStart(uint64_t currentLineStartByte,
                                             uint64_t& outPrevLineStartByte,
                                             std::string& errOut) const {
    if (currentLineStartByte <= contentStartByte_)
        return false;

    uint64_t scanEnd = currentLineStartByte;

    unsigned char trailingByte = 0;
    if (!provMgr_->readFile(path_, scanEnd - 1, 1, &trailingByte, errOut))
        return false;

    if (trailingByte == '\n') {
        --scanEnd;
        if (scanEnd > contentStartByte_) {
            unsigned char maybeCr = 0;
            if (!provMgr_->readFile(path_, scanEnd - 1, 1, &maybeCr, errOut))
                return false;
            if (maybeCr == '\r')
                --scanEnd;
        }
    } else if (trailingByte == '\r') {
        --scanEnd;
    }

    if (scanEnd <= contentStartByte_) {
        outPrevLineStartByte = contentStartByte_;
        return true;
    }

    while (scanEnd > contentStartByte_) {
        const uint64_t chunkStart =
            (scanEnd > static_cast<uint64_t>(kScanChunkSize))
                ? std::max<uint64_t>(contentStartByte_, scanEnd - kScanChunkSize)
                : contentStartByte_;
        const std::size_t chunkSize = static_cast<std::size_t>(scanEnd - chunkStart);
        std::vector<unsigned char> chunk(chunkSize);
        if (!provMgr_->readFile(path_, chunkStart, chunkSize, chunk.data(), errOut))
            return false;

        for (std::size_t i = chunkSize; i > 0; --i) {
            const unsigned char ch = chunk[i - 1];
            if (ch == '\n' || ch == '\r') {
                const uint64_t newlineOffset = chunkStart + static_cast<uint64_t>(i - 1);
                if (ch == '\r') {
                    bool hasFollowingLf = false;
                    if (newlineOffset + 1 < scanEnd) {
                        unsigned char nextByte = 0;
                        if (i < chunkSize) {
                            nextByte = chunk[i];
                        } else if (!provMgr_->readFile(path_, newlineOffset + 1, 1, &nextByte,
                                                       errOut)) {
                            return false;
                        }
                        hasFollowingLf = nextByte == '\n';
                    }
                    if (hasFollowingLf)
                        continue;
                }
                outPrevLineStartByte = newlineOffset + 1;
                return true;
            }
        }
        scanEnd = chunkStart;
    }

    outPrevLineStartByte = contentStartByte_;
    return true;
}

bool TextEditorScreen::findPreviousPageAnchor(PageAnchor& outAnchor, std::string& errOut) const {
    outAnchor.startByte = pageStartByte_;
    outAnchor.startLineNumber = pageStartLineNumber_;
    if (pageStartByte_ <= contentStartByte_ || pageStartLineNumber_ == 0)
        return false;

    const int rows = visibleRowCount();
    for (int i = 0; i < rows; ++i) {
        uint64_t prevStart = 0;
        if (!findPreviousLineStart(outAnchor.startByte, prevStart, errOut))
            return false;
        if (prevStart == outAnchor.startByte)
            break;
        outAnchor.startByte = prevStart;
        if (outAnchor.startLineNumber > 0)
            --outAnchor.startLineNumber;
        if (outAnchor.startByte <= contentStartByte_ || outAnchor.startLineNumber == 0)
            break;
    }

    return outAnchor.startByte != pageStartByte_ || outAnchor.startLineNumber != pageStartLineNumber_;
}

bool TextEditorScreen::pageDown(std::string& errOut) {
    if (pageLines_.empty())
        return true;

    const LineView& last = pageLines_.back();
    if (last.lineStartByte == fileSize_ && last.lineByteLength == 0 && last.newlineByteLength == 0)
        return true;
    const uint64_t nextStart = last.endByteExclusive();
    if (nextStart > fileSize_ || (nextStart == fileSize_ && !fileEndsWithNewline_))
        return true;

    pageHistory_.push_back({pageStartByte_, pageStartLineNumber_});
    return loadPage(nextStart, last.lineNumber + 1, 0, false, errOut);
}

bool TextEditorScreen::pageUp(bool cursorToLastLine, std::string& errOut) {
    PageAnchor anchor;
    if (!pageHistory_.empty()) {
        anchor = pageHistory_.back();
        pageHistory_.pop_back();
    } else if (!findPreviousPageAnchor(anchor, errOut)) {
        return true;
    }

    if (!loadPage(anchor.startByte, anchor.startLineNumber, 0, false, errOut))
        return false;
    if (cursorToLastLine && !pageLines_.empty())
        cursorIndex_ = static_cast<int>(pageLines_.size()) - 1;
    return true;
}

bool TextEditorScreen::moveCursorUp(std::string& errOut) {
    if (pageLines_.empty())
        return true;
    if (cursorIndex_ > 0) {
        --cursorIndex_;
        return true;
    }
    return pageUp(true, errOut);
}

bool TextEditorScreen::moveCursorDown(std::string& errOut) {
    if (pageLines_.empty())
        return true;
    if (cursorIndex_ + 1 < static_cast<int>(pageLines_.size())) {
        ++cursorIndex_;
        return true;
    }
    return pageDown(errOut);
}

const TextEditorScreen::LineView* TextEditorScreen::currentLine() const {
    if (cursorIndex_ < 0 || cursorIndex_ >= static_cast<int>(pageLines_.size()))
        return nullptr;
    return &pageLines_[cursorIndex_];
}

bool TextEditorScreen::lineSelected(uint64_t lineNumber) const {
    if (!selection_.active)
        return false;
    return lineNumber >= selection_.startLine && lineNumber <= selection_.endLine;
}

bool TextEditorScreen::selectionCoversAllLines() const {
    if (!selection_.active)
        return false;
    return selection_.startByte <= contentStartByte_ && selection_.endByte >= fileSize_;
}

bool TextEditorScreen::currentLineIsOnlyLineWithoutNewline() const {
    const LineView* line = currentLine();
    if (!line)
        return false;
    return line->lineStartByte <= contentStartByte_ && line->endByteExclusive() >= fileSize_ &&
           line->newlineByteLength == 0;
}

void TextEditorScreen::toggleSelectCurrentLine() {
    const LineView* line = currentLine();
    if (!line)
        return;

    const uint64_t startByte = line->lineStartByte;
    const uint64_t endByte = line->endByteExclusive();

    if (!selection_.active) {
        selection_.active = true;
        selection_.startLine = line->lineNumber;
        selection_.endLine = line->lineNumber;
        selection_.startByte = startByte;
        selection_.endByte = endByte;
        selection_.lastLineNewlineByteLength = line->newlineByteLength;
        return;
    }

    if (line->lineNumber == selection_.endLine + 1 && startByte == selection_.endByte) {
        selection_.endLine = line->lineNumber;
        selection_.endByte = endByte;
        selection_.lastLineNewlineByteLength = line->newlineByteLength;
        return;
    }

    if (selection_.startLine > 0 && line->lineNumber + 1 == selection_.startLine &&
        endByte == selection_.startByte) {
        selection_.startLine = line->lineNumber;
        selection_.startByte = startByte;
        return;
    }

    selection_.active = true;
    selection_.startLine = line->lineNumber;
    selection_.endLine = line->lineNumber;
    selection_.startByte = startByte;
    selection_.endByte = endByte;
    selection_.lastLineNewlineByteLength = line->newlineByteLength;
}

int TextEditorScreen::hitTestRow(int y) const {
    const int top = contentTop();
    const int localY = y - top;
    if (localY < 0 || localY >= contentHeight())
        return -1;
    const int index = localY / kRowHeight;
    if (index < 0 || index >= static_cast<int>(pageLines_.size()))
        return -1;
    return index;
}

TextEditorAction TextEditorScreen::handleInput(uint64_t kDown, const TouchTap* tap) {
    if (!active_)
        return TextEditorAction::None;

    const int menuHitX = theme::SCREEN_W - kHeaderEdgePadding - kHeaderHitW;
    const int hitY = (theme::HEADER_H - kHeaderHitH) / 2;

    if (tap && tap->active) {
        if (pointInRect(tap, menuHitX, hitY, kHeaderHitW, kHeaderHitH))
            return TextEditorAction::OpenMenu;

        const int row = hitTestRow(tap->y);
        if (row >= 0) {
            if (row == cursorIndex_)
                return TextEditorAction::EditCurrentLine;
            cursorIndex_ = row;
            return TextEditorAction::None;
        }
    }

    if (kDown & HidNpadButton_B)
        return TextEditorAction::Close;
    if (kDown & HidNpadButton_Plus)
        return TextEditorAction::OpenMenu;
    if (kDown & HidNpadButton_A)
        return TextEditorAction::EditCurrentLine;
    if (kDown & HidNpadButton_Y) {
        toggleSelectCurrentLine();
        std::string err;
        if (!moveCursorDown(err))
            pendingError_ = std::move(err);
        return TextEditorAction::None;
    }
    if (kDown & HidNpadButton_X) {
        selection_.clear();
        return TextEditorAction::None;
    }
    if (kDown & HidNpadButton_AnyUp) {
        std::string err;
        if (!moveCursorUp(err))
            pendingError_ = std::move(err);
        return TextEditorAction::None;
    }
    if (kDown & HidNpadButton_AnyDown) {
        std::string err;
        if (!moveCursorDown(err))
            pendingError_ = std::move(err);
        return TextEditorAction::None;
    }
    if (kDown & HidNpadButton_AnyLeft) {
        std::string err;
        if (!pageUp(false, err))
            pendingError_ = std::move(err);
        return TextEditorAction::None;
    }
    if (kDown & HidNpadButton_AnyRight) {
        std::string err;
        if (!pageDown(err))
            pendingError_ = std::move(err);
        return TextEditorAction::None;
    }

    return TextEditorAction::None;
}

void TextEditorScreen::render(Renderer& renderer, FontManager& fm, const I18n& i18n,
                              SDL_Texture* menuIcon, bool touchButtonsEnabled) const {
    if (!active_)
        return;

    renderer.clear(theme::BG);
    renderer.drawRectFilled(0, 0, theme::SCREEN_W, theme::HEADER_H, theme::HEADER_BG);
    renderer.drawRectFilled(0, theme::HEADER_H - 1, theme::SCREEN_W, 1, theme::DIVIDER);

    renderer.drawRectFilled(0, footerY(), theme::SCREEN_W, theme::FOOTER_H, theme::HEADER_BG);
    renderer.drawRectFilled(0, footerY(), theme::SCREEN_W, 1, theme::DIVIDER);

    const char* title = i18n.t(readOnly_ ? "text_editor.title_preview" : "text_editor.title");
    fm.drawText(renderer.sdl(), title, theme::PADDING, 5, theme::FONT_SIZE_TITLE,
                theme::PRIMARY);

    const int menuHitX = theme::SCREEN_W - kHeaderEdgePadding - kHeaderHitW;
    const int menuIconX = menuHitX + (kHeaderHitW - kHeaderIconSize) / 2;
    const int iconY = (theme::HEADER_H - kHeaderIconSize) / 2;
    if (menuIcon)
        renderer.drawTexture(menuIcon, menuIconX, iconY, kHeaderIconSize, kHeaderIconSize);

    int pathRight = menuHitX - 12;
    if (readOnly_) {
        const char* readonlyText = i18n.t("text_editor.read_only");
        const int readonlyW = fm.measureText(readonlyText, theme::FONT_SIZE_SMALL);
        const int readonlyY = (theme::HEADER_H - fm.fontHeight(theme::FONT_SIZE_SMALL)) / 2;
        const int readonlyX = menuHitX - 12 - readonlyW;
        fm.drawText(renderer.sdl(), readonlyText, readonlyX, readonlyY,
                    theme::FONT_SIZE_SMALL, theme::TEXT_DISABLED);
        pathRight = readonlyX - 12;
    }

    const int pathMaxW = pathRight - theme::PADDING;
    if (pathMaxW > 40) {
        fm.drawTextEllipsis(renderer.sdl(), displayPath_.c_str(), theme::PADDING, kHeaderPathTop,
                            theme::FONT_SIZE_SMALL, theme::TEXT_DISABLED, pathMaxW);
    }

    const TouchButtonsLayout touchLayout = touchButtonsLayout(touchButtonsEnabled);
    if (touchLayout.enabled) {
        const SDL_Color buttonBg = theme::SURFACE;
        const SDL_Color buttonBorder = theme::DIVIDER;
        renderer.drawRoundedRectFilled(touchLayout.prevX, touchLayout.y, touchLayout.pageW,
                                       touchLayout.h, 8, buttonBg);
        renderer.drawRoundedRect(touchLayout.prevX, touchLayout.y, touchLayout.pageW,
                                 touchLayout.h, 8, buttonBorder);
        renderer.drawRoundedRectFilled(touchLayout.nextX, touchLayout.y, touchLayout.pageW,
                                       touchLayout.h, 8, buttonBg);
        renderer.drawRoundedRect(touchLayout.nextX, touchLayout.y, touchLayout.pageW,
                                 touchLayout.h, 8, buttonBorder);
        renderer.drawRoundedRectFilled(touchLayout.selectX, touchLayout.y, touchLayout.selectW,
                                       touchLayout.h, 8, buttonBg);
        renderer.drawRoundedRect(touchLayout.selectX, touchLayout.y, touchLayout.selectW,
                                 touchLayout.h, 8, buttonBorder);
        drawThickChevron(renderer, touchLayout.prevX, touchLayout.y, touchLayout.pageW,
                         touchLayout.h, true, theme::PRIMARY);
        drawThickChevron(renderer, touchLayout.nextX, touchLayout.y, touchLayout.pageW,
                         touchLayout.h, false, theme::PRIMARY);
        drawSelectIcon(renderer, touchLayout.selectX, touchLayout.y, touchLayout.selectW,
                       touchLayout.h, theme::PRIMARY);
    }

    int footerTextH = fm.fontHeight(theme::FONT_SIZE_FOOTER);
    int footerTextY = footerY() + (theme::FOOTER_H - footerTextH) / 2 - 1;
    const char* tips[][2] = {
        {"A", i18n.t("text_editor.footer_edit")},
        {"B", i18n.t("text_editor.footer_close")},
        {"Y", i18n.t("text_editor.footer_select")},
        {"X", i18n.t("text_editor.footer_clear_selection")},
        {"+", i18n.t("text_editor.footer_menu")},
    };
    constexpr int tipCount = 5;
    int totalWidth = 0;
    for (int i = 0; i < tipCount; ++i) {
        const auto& tip = tips[i];
        totalWidth += fm.measureText(tip[0], theme::FONT_SIZE_FOOTER);
        totalWidth += fm.measureText(":", theme::FONT_SIZE_FOOTER);
        totalWidth += fm.measureText(tip[1], theme::FONT_SIZE_FOOTER);
    }
    const int gap = 28;
    totalWidth += gap * (tipCount - 1);
    const int contentLeft = touchLayout.enabled ? touchLayout.contentLeft : theme::PADDING;
    const int contentRight =
        touchLayout.enabled ? touchLayout.contentRight : (theme::SCREEN_W - theme::PADDING);
    const int availableW = std::max(0, contentRight - contentLeft);
    int footerX = contentLeft + std::max(0, (availableW - totalWidth) / 2);
    for (int i = 0; i < tipCount; ++i) {
        drawFooterTip(renderer, fm, footerX, footerTextY, tips[i][0], tips[i][1]);
        if (i + 1 < tipCount)
            footerX += gap;
    }

    const int top = contentTop();
    const int lineDigitsWidth =
        fm.measureText(zeroPaddedLineNumber(pageLines_.empty() ? 0 : pageLines_.back().lineNumber).c_str(),
                       kLineNumberFontSize);
    const int dividerX = kContentPadX + kLineNumberLeftPad + lineDigitsWidth + kLineNumberRightGap;
    const int textX = dividerX + kTextLeftPad;
    const int contentBottom = top + contentHeight();
    renderer.drawRectFilled(dividerX, top, 1, contentHeight(), theme::DIVIDER);

    for (int i = 0; i < static_cast<int>(pageLines_.size()); ++i) {
        const LineView& line = pageLines_[i];
        const int rowY = top + i * kRowHeight;
        if (rowY + kRowHeight > contentBottom)
            break;

        if (lineSelected(line.lineNumber)) {
            renderer.drawRectFilled(kContentPadX, rowY, theme::SCREEN_W - kContentPadX * 2,
                                    kRowHeight, theme::SELECTED_ROW);
            renderer.drawRectFilled(kContentPadX, rowY, kSelectionBarW, kRowHeight,
                                    theme::SELECTED_BAR);
        }

        if (i == cursorIndex_) {
            renderer.drawRectFilled(kContentPadX, rowY, theme::SCREEN_W - kContentPadX * 2,
                                    kRowHeight, theme::CURSOR_ROW);
        }

        const std::string lineNo = zeroPaddedLineNumber(line.lineNumber);
        const int numberAreaLeft = kContentPadX + kLineNumberLeftPad;
        const int numberAreaRight = dividerX - kLineNumberRightGap;
        const int numberTextW = fm.measureText(lineNo.c_str(), kLineNumberFontSize);
        const int numberAreaW = std::max(0, numberAreaRight - numberAreaLeft);
        const int numberTextX = numberAreaLeft + std::max(0, (numberAreaW - numberTextW) / 2);
        const int numberTextY =
            rowY + (kRowHeight - fm.fontHeight(kLineNumberFontSize)) / 2;
        const SDL_Color numberColor = lineSelected(line.lineNumber)
            ? theme::PRIMARY
            : theme::TEXT_SECONDARY;
        fm.drawText(renderer.sdl(), lineNo.c_str(), numberTextX, numberTextY,
                    kLineNumberFontSize, numberColor);

        const int textMaxW = theme::SCREEN_W - textX - theme::PADDING;
        if (textMaxW > 20) {
            const int textY = rowY + (kRowHeight - fm.fontHeight(kTextFontSize)) / 2;
            fm.drawTextEllipsis(renderer.sdl(), line.previewText.c_str(), textX, textY,
                                kTextFontSize, theme::TEXT, textMaxW);
        }
    }
}

std::vector<ModalOptionListEntry> TextEditorScreen::buildMenuOptions(const I18n& i18n) const {
    const bool hasLine = currentLine() != nullptr;
    const bool canModify = hasLine && !readOnly_;
    const bool hasClipboard = !textClipboard_.empty();
    return {
        {i18n.t("text_editor.menu_copy"), hasLine},
        {i18n.t("text_editor.menu_cut"), canModify},
        {i18n.t("text_editor.menu_paste_before"), canModify && hasClipboard},
        {i18n.t("text_editor.menu_paste_after"), canModify && hasClipboard},
        {i18n.t("text_editor.menu_view_clipboard"), hasClipboard},
        {i18n.t("text_editor.menu_insert_before"), canModify},
        {i18n.t("text_editor.menu_insert_after"), canModify},
        {i18n.t(selection_.active ? "text_editor.menu_delete_selected"
                                  : "text_editor.menu_delete_current"),
         canModify},
        {i18n.t("modal.cancel"), true},
    };
}

bool TextEditorScreen::readRangeToString(uint64_t offset, uint64_t size, std::string& out,
                                         std::string& errOut) const {
    out.clear();
    if (size == 0)
        return true;
    out.reserve(static_cast<std::size_t>(std::min<uint64_t>(size, 4096)));

    uint64_t remaining = size;
    uint64_t currentOffset = offset;
    std::vector<char> chunk(kRewriteChunkSize);
    while (remaining > 0) {
        const std::size_t readSize = static_cast<std::size_t>(
            std::min<uint64_t>(remaining, static_cast<uint64_t>(chunk.size())));
        if (!provMgr_->readFile(path_, currentOffset, readSize, chunk.data(), errOut))
            return false;
        out.append(chunk.data(), readSize);
        currentOffset += readSize;
        remaining -= readSize;
    }
    return true;
}

bool TextEditorScreen::copySelectionToClipboard(std::string& errOut) {
    uint64_t startByte = 0;
    uint64_t endByte = 0;
    uint8_t lastNewlineLength = 0;

    if (selection_.active) {
        startByte = selection_.startByte;
        endByte = selection_.endByte;
        lastNewlineLength = selection_.lastLineNewlineByteLength;
    } else {
        const LineView* line = currentLine();
        if (!line) {
            errOut = "missing line";
            return false;
        }
        startByte = line->lineStartByte;
        endByte = line->endByteExclusive();
        lastNewlineLength = line->newlineByteLength;
    }

    if (!readRangeToString(startByte, endByte - startByte, textClipboard_, errOut))
        return false;
    if (lastNewlineLength == 0)
        textClipboard_.push_back('\n');
    return true;
}

fs::FileProvider* TextEditorScreen::resolveWritableProvider(std::string& relPath,
                                                            std::string& errOut) const {
    relPath.clear();
    if (!provMgr_) {
        errOut = "missing provider manager";
        return nullptr;
    }
    fs::FileProvider* provider = provMgr_->resolveProvider(path_, relPath);
    if (!provider) {
        errOut = "missing provider";
        return nullptr;
    }
    if (provider->isReadOnly()) {
        errOut = "read_only";
        return nullptr;
    }
    return provider;
}

bool TextEditorScreen::writeSegmentsToTempPath(const std::string& tempRelPath,
                                               const std::vector<RewriteSegment>& segments,
                                               std::string& errOut) {
    std::string relPath;
    fs::FileProvider* provider = resolveWritableProvider(relPath, errOut);
    if (!provider)
        return false;
    (void)relPath;

    RewriteStream stream(*provMgr_, path_, segments);
    const uint64_t totalSize = stream.totalSize();

    if (provider->supportsPartialWrite()) {
        if (totalSize == 0)
            return provider->writeFileChunk(tempRelPath, 0, nullptr, 0, true, errOut);

        std::vector<unsigned char> buffer(kRewriteChunkSize);
        uint64_t offset = 0;
        while (offset < totalSize) {
            const std::size_t chunk = static_cast<std::size_t>(
                std::min<uint64_t>(totalSize - offset, static_cast<uint64_t>(buffer.size())));
            if (!stream.read(buffer.data(), chunk, errOut))
                return false;
            if (!provider->writeFileChunk(tempRelPath, offset, buffer.data(), chunk,
                                          offset == 0, errOut)) {
                return false;
            }
            offset += chunk;
        }
        return true;
    }

    if (provider->kind() == fs::ProviderKind::WebDav) {
        auto* webdav = static_cast<fs::WebDavProvider*>(provider);
        if (totalSize == 0)
            return webdav->writeFile(tempRelPath, nullptr, 0, errOut);
        return webdav->uploadFromStream(
            tempRelPath, totalSize,
            [&stream](void* outBuffer, size_t size, uint64_t offset, std::string& streamErr) {
                (void)offset;
                return stream.read(outBuffer, size, streamErr);
            },
            errOut);
    }

    errOut = "provider does not support streamed text rewrite";
    return false;
}

bool TextEditorScreen::rewriteFile(const std::vector<RewriteSegment>& segments, uint64_t anchorByte,
                                   uint64_t anchorLineNumber, int preferredCursor,
                                   std::string& errOut) {
    std::string relPath;
    fs::FileProvider* provider = resolveWritableProvider(relPath, errOut);
    if (!provider)
        return false;

    const std::string dir = parentDir(path_);
    const std::string name = baseName(path_);
    const std::string tempPath = dir + name + ".tmp";
    const std::string backupPath = dir + name + ".tmp.swap";
    const std::string tempRelPath = parentDir(relPath) + baseName(tempPath);
    const std::string backupRelPath = parentDir(relPath) + baseName(backupPath);

    if (provMgr_->pathExists(tempPath)) {
        std::string cleanupErr;
        if (!provMgr_->removeAll(tempPath, cleanupErr) && errOut.empty())
            errOut = cleanupErr;
    }
    if (provMgr_->pathExists(backupPath)) {
        std::string cleanupErr;
        provMgr_->removeAll(backupPath, cleanupErr);
    }

    if (!writeSegmentsToTempPath(tempRelPath, segments, errOut)) {
        std::string cleanupErr;
        provMgr_->removeAll(tempPath, cleanupErr);
        return false;
    }

    const bool originalExists = provMgr_->pathExists(path_);
    bool originalMoved = false;
    if (originalExists) {
        if (!provider->renamePath(relPath, backupRelPath, errOut)) {
            std::string cleanupErr;
            provMgr_->removeAll(tempPath, cleanupErr);
            return false;
        }
        originalMoved = true;
    }

    if (!provider->renamePath(tempRelPath, relPath, errOut)) {
        std::string restoreErr;
        if (originalMoved)
            provider->renamePath(backupRelPath, relPath, restoreErr);
        std::string cleanupErr;
        provMgr_->removeAll(tempPath, cleanupErr);
        return false;
    }

    if (originalMoved) {
        std::string cleanupErr;
        provMgr_->removeAll(backupPath, cleanupErr);
    }

    selection_.clear();
    pageHistory_.clear();

    contentStartByte_ = 0;
    if (!refreshFileStat(errOut) || !detectBom(errOut))
        return false;

    uint64_t loadAnchorByte = std::max<uint64_t>(anchorByte, contentStartByte_);
    if (loadAnchorByte > fileSize_)
        loadAnchorByte = fileSize_;

    if (!loadPage(loadAnchorByte, anchorLineNumber, preferredCursor, true, errOut)) {
        uint64_t fallbackByte = std::min<uint64_t>(pageStartByte_, fileSize_);
        uint64_t fallbackLine = pageStartLineNumber_;
        if (fallbackByte == fileSize_ && fileSize_ > contentStartByte_ && !fileEndsWithNewline_) {
            fallbackByte = contentStartByte_;
            fallbackLine = 0;
        }
        if (fallbackByte <= contentStartByte_)
            fallbackLine = 0;
        if (!loadPage(fallbackByte, fallbackLine, 0, true, errOut))
            return false;
    }
    return true;
}

bool TextEditorScreen::cutSelection(std::string& errOut) {
    if (!copySelectionToClipboard(errOut))
        return false;

    uint64_t startByte = 0;
    uint64_t endByte = 0;
    uint64_t startLine = 0;
    if (selection_.active) {
        startByte = selection_.startByte;
        endByte = selection_.endByte;
        startLine = selection_.startLine;
    } else {
        const LineView* line = currentLine();
        if (!line) {
            errOut = "missing line";
            return false;
        }
        startByte = line->lineStartByte;
        endByte = line->endByteExclusive();
        startLine = line->lineNumber;
    }

    std::vector<RewriteSegment> segments;
    if (startByte > 0)
        segments.push_back({RewriteSegment::Kind::SourceRange, 0, startByte, {}});
    if (endByte < fileSize_)
        segments.push_back({RewriteSegment::Kind::SourceRange, endByte, fileSize_ - endByte, {}});

    const bool preservePageAnchor = startByte > pageStartByte_;
    const uint64_t anchorByte = preservePageAnchor ? pageStartByte_ : startByte;
    const uint64_t anchorLine = preservePageAnchor ? pageStartLineNumber_ : startLine;
    const int preferredCursor = preservePageAnchor ? cursorIndex_ : 0;
    return rewriteFile(segments, anchorByte, anchorLine, preferredCursor, errOut);
}

bool TextEditorScreen::pasteBeforeCurrentLine(std::string& errOut) {
    const LineView* line = currentLine();
    if (!line) {
        errOut = "missing line";
        return false;
    }

    std::vector<RewriteSegment> segments;
    if (line->lineStartByte > 0)
        segments.push_back({RewriteSegment::Kind::SourceRange, 0, line->lineStartByte, {}});
    segments.push_back({RewriteSegment::Kind::Literal, 0,
                        static_cast<uint64_t>(textClipboard_.size()), textClipboard_});
    if (line->lineStartByte < fileSize_) {
        segments.push_back({RewriteSegment::Kind::SourceRange, line->lineStartByte,
                            fileSize_ - line->lineStartByte, {}});
    }
    const bool preservePageAnchor = line->lineStartByte > pageStartByte_;
    const uint64_t anchorByte = preservePageAnchor ? pageStartByte_ : line->lineStartByte;
    const uint64_t anchorLine = preservePageAnchor ? pageStartLineNumber_ : line->lineNumber;
    const int preferredCursor = preservePageAnchor ? cursorIndex_ : 0;
    return rewriteFile(segments, anchorByte, anchorLine, preferredCursor, errOut);
}

bool TextEditorScreen::pasteAfterCurrentLine(std::string& errOut) {
    const LineView* line = currentLine();
    if (!line) {
        errOut = "missing line";
        return false;
    }

    const bool needsLeadingNewline = line->newlineByteLength == 0;
    const uint64_t insertOffset = line->endByteExclusive();
    std::vector<RewriteSegment> segments;
    if (insertOffset > 0)
        segments.push_back({RewriteSegment::Kind::SourceRange, 0, insertOffset, {}});
    if (needsLeadingNewline)
        segments.push_back({RewriteSegment::Kind::Literal, 0, 1, "\n"});
    segments.push_back({RewriteSegment::Kind::Literal, 0,
                        static_cast<uint64_t>(textClipboard_.size()), textClipboard_});
    if (insertOffset < fileSize_)
        segments.push_back({RewriteSegment::Kind::SourceRange, insertOffset,
                            fileSize_ - insertOffset, {}});

    return rewriteFile(segments, pageStartByte_, pageStartLineNumber_, cursorIndex_, errOut);
}

bool TextEditorScreen::insertEmptyLineBefore(std::string& errOut) {
    const LineView* line = currentLine();
    if (!line) {
        errOut = "missing line";
        return false;
    }

    std::vector<RewriteSegment> segments;
    if (line->lineStartByte > 0)
        segments.push_back({RewriteSegment::Kind::SourceRange, 0, line->lineStartByte, {}});
    segments.push_back({RewriteSegment::Kind::Literal, 0, 1, "\n"});
    if (line->lineStartByte < fileSize_)
        segments.push_back({RewriteSegment::Kind::SourceRange, line->lineStartByte,
                            fileSize_ - line->lineStartByte, {}});

    const bool preservePageAnchor = line->lineStartByte > pageStartByte_;
    const uint64_t anchorByte = preservePageAnchor ? pageStartByte_ : line->lineStartByte;
    const uint64_t anchorLine = preservePageAnchor ? pageStartLineNumber_ : line->lineNumber;
    const int preferredCursor = preservePageAnchor ? cursorIndex_ : 0;
    return rewriteFile(segments, anchorByte, anchorLine, preferredCursor, errOut);
}

bool TextEditorScreen::insertEmptyLineAfter(std::string& errOut) {
    const LineView* line = currentLine();
    if (!line) {
        errOut = "missing line";
        return false;
    }

    const uint64_t insertOffset = line->endByteExclusive();
    std::vector<RewriteSegment> segments;
    if (insertOffset > 0)
        segments.push_back({RewriteSegment::Kind::SourceRange, 0, insertOffset, {}});
    segments.push_back({RewriteSegment::Kind::Literal, 0, 1, "\n"});
    if (insertOffset < fileSize_)
        segments.push_back({RewriteSegment::Kind::SourceRange, insertOffset,
                            fileSize_ - insertOffset, {}});

    return rewriteFile(segments, pageStartByte_, pageStartLineNumber_, cursorIndex_, errOut);
}

bool TextEditorScreen::deleteSelectedOrCurrentLines(std::string& errOut) {
    if (readOnly_) {
        errOut = "read_only";
        return false;
    }

    uint64_t startByte = 0;
    uint64_t endByte = 0;
    uint64_t startLine = 0;
    bool clearContent = false;

    if (selection_.active) {
        startByte = selection_.startByte;
        endByte = selection_.endByte;
        startLine = selection_.startLine;
        clearContent = selectionCoversAllLines();
    } else {
        const LineView* line = currentLine();
        if (!line) {
            errOut = "missing line";
            return false;
        }
        startByte = line->lineStartByte;
        endByte = line->endByteExclusive();
        startLine = line->lineNumber;
        clearContent = currentLineIsOnlyLineWithoutNewline();
    }

    std::vector<RewriteSegment> segments;
    if (clearContent) {
        if (contentStartByte_ > 0)
            segments.push_back({RewriteSegment::Kind::SourceRange, 0, contentStartByte_, {}});
        return rewriteFile(segments, contentStartByte_, 0, 0, errOut);
    }

    if (startByte > 0)
        segments.push_back({RewriteSegment::Kind::SourceRange, 0, startByte, {}});
    if (endByte < fileSize_)
        segments.push_back({RewriteSegment::Kind::SourceRange, endByte, fileSize_ - endByte, {}});

    const bool preservePageAnchor = startByte > pageStartByte_;
    const uint64_t anchorByte = preservePageAnchor ? pageStartByte_ : startByte;
    const uint64_t anchorLine = preservePageAnchor ? pageStartLineNumber_ : startLine;
    const int preferredCursor = preservePageAnchor ? cursorIndex_ : 0;
    return rewriteFile(segments, anchorByte, anchorLine, preferredCursor, errOut);
}

bool TextEditorScreen::runMenuAction(TextEditorMenuAction action, bool& openClipboard,
                                     std::string& errOut) {
    openClipboard = false;

    switch (action) {
    case TextEditorMenuAction::Copy:
        return copySelectionToClipboard(errOut);
    case TextEditorMenuAction::Cut:
        if (readOnly_) {
            errOut = "read_only";
            return false;
        }
        return cutSelection(errOut);
    case TextEditorMenuAction::PasteBefore:
        if (readOnly_) {
            errOut = "read_only";
            return false;
        }
        return pasteBeforeCurrentLine(errOut);
    case TextEditorMenuAction::PasteAfter:
        if (readOnly_) {
            errOut = "read_only";
            return false;
        }
        return pasteAfterCurrentLine(errOut);
    case TextEditorMenuAction::ViewClipboard:
        openClipboard = true;
        return true;
    case TextEditorMenuAction::InsertBefore:
        if (readOnly_) {
            errOut = "read_only";
            return false;
        }
        return insertEmptyLineBefore(errOut);
    case TextEditorMenuAction::InsertAfter:
        if (readOnly_) {
            errOut = "read_only";
            return false;
        }
        return insertEmptyLineAfter(errOut);
    case TextEditorMenuAction::DeleteLines:
        if (readOnly_) {
            errOut = "read_only";
            return false;
        }
        return deleteSelectedOrCurrentLines(errOut);
    case TextEditorMenuAction::Cancel:
    default:
        return true;
    }
}

std::string TextEditorScreen::clipboardBody(const I18n& i18n) const {
    if (textClipboard_.empty())
        return i18n.t("text_editor.clipboard_empty");
    return sanitizeUtf8ForDisplay(textClipboard_);
}

void TextEditorScreen::clearClipboard() {
    textClipboard_.clear();
}

bool TextEditorScreen::currentLineInputText(std::string& outText, std::string& errOut) const {
    const LineView* line = currentLine();
    if (!line) {
        errOut = "missing line";
        return false;
    }
    std::string raw;
    if (!readRangeToString(line->lineStartByte, line->lineByteLength, raw, errOut))
        return false;
    outText = sanitizeUtf8ForDisplay(raw);
    return true;
}

bool TextEditorScreen::replaceCurrentLine(const std::string& newText, std::string& errOut) {
    if (readOnly_) {
        errOut = "read_only";
        return false;
    }

    const LineView* line = currentLine();
    if (!line) {
        errOut = "missing line";
        return false;
    }

    std::vector<RewriteSegment> segments;
    if (line->lineStartByte > 0)
        segments.push_back({RewriteSegment::Kind::SourceRange, 0, line->lineStartByte, {}});
    std::string replacement = newText;
    replacement.push_back('\n');
    segments.push_back({RewriteSegment::Kind::Literal, 0,
                        static_cast<uint64_t>(replacement.size()), replacement});
    const uint64_t suffixOffset = line->lineStartByte + line->lineByteLength +
                                  static_cast<uint64_t>(line->newlineByteLength);
    if (suffixOffset < fileSize_)
        segments.push_back({RewriteSegment::Kind::SourceRange, suffixOffset,
                            fileSize_ - suffixOffset, {}});

    selection_.clear();
    return rewriteFile(segments, pageStartByte_, pageStartLineNumber_, cursorIndex_, errOut);
}

} // namespace xxplore
