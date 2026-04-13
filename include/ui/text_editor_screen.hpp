#pragma once

#include "ui/modals.hpp"
#include <SDL.h>
#include <cstdint>
#include <string>
#include <vector>

namespace xxplore {

class FontManager;
class I18n;
class Renderer;
struct TouchTap;

namespace fs {
class FileProvider;
class ProviderManager;
}

enum class TextEditorAction {
    None,
    Close,
    EditCurrentLine,
    OpenMenu,
};

enum class TextEditorMenuAction {
    Copy = 0,
    Cut,
    PasteBefore,
    PasteAfter,
    ViewClipboard,
    InsertBefore,
    InsertAfter,
    Cancel,
};

class TextEditorScreen {
public:
    struct RewriteSegment {
        enum class Kind {
            SourceRange,
            Literal,
        };

        Kind        kind = Kind::SourceRange;
        uint64_t    sourceOffset = 0;
        uint64_t    length = 0;
        std::string literal;
    };

    bool open(fs::ProviderManager& provMgr, const std::string& path, std::string& errOut);
    void close();

    bool isOpen() const { return active_; }
    bool isReadOnly() const { return readOnly_; }
    bool clipboardEmpty() const { return textClipboard_.empty(); }
    const std::string& path() const { return path_; }

    TextEditorAction handleInput(uint64_t kDown, const TouchTap* tap = nullptr);
    void render(Renderer& renderer, FontManager& fm, const I18n& i18n,
                SDL_Texture* menuIcon, bool touchButtonsEnabled) const;
    bool takePendingError(std::string& errOut);

    std::vector<ModalOptionListEntry> buildMenuOptions(const I18n& i18n) const;
    bool runMenuAction(TextEditorMenuAction action, bool& openClipboard, std::string& errOut);
    std::string clipboardBody(const I18n& i18n) const;
    void clearClipboard();

    bool currentLineInputText(std::string& outText, std::string& errOut) const;
    bool replaceCurrentLine(const std::string& newText, std::string& errOut);

private:
    struct LineView {
        uint64_t    lineNumber = 0;
        uint64_t    lineStartByte = 0;
        uint64_t    lineByteLength = 0;
        uint8_t     newlineByteLength = 0;
        std::string previewText;

        uint64_t endByteExclusive() const {
            return lineStartByte + lineByteLength + static_cast<uint64_t>(newlineByteLength);
        }
    };

    struct PageAnchor {
        uint64_t startByte = 0;
        uint64_t startLineNumber = 0;
    };

    struct SelectionState {
        bool     active = false;
        uint64_t startLine = 0;
        uint64_t endLine = 0;
        uint64_t startByte = 0;
        uint64_t endByte = 0;
        uint8_t  lastLineNewlineByteLength = 0;

        void clear() {
            active = false;
            startLine = 0;
            endLine = 0;
            startByte = 0;
            endByte = 0;
            lastLineNewlineByteLength = 0;
        }
    };

    bool detectBom(std::string& errOut);
    bool refreshFileStat(std::string& errOut);
    bool loadPage(uint64_t startByte, uint64_t startLineNumber, int cursorIndex,
                  bool resetHistory, std::string& errOut);
    bool scanPage(uint64_t startByte, uint64_t startLineNumber, std::vector<LineView>& outLines,
                  std::string& errOut) const;
    bool scanLine(uint64_t startByte, uint64_t lineNumber, LineView& outLine,
                  std::string& errOut) const;
    bool pageDown(std::string& errOut);
    bool pageUp(bool cursorToLastLine, std::string& errOut);
    bool moveCursorUp(std::string& errOut);
    bool moveCursorDown(std::string& errOut);
    bool findPreviousLineStart(uint64_t currentLineStartByte, uint64_t& outPrevLineStartByte,
                               std::string& errOut) const;
    bool findPreviousPageAnchor(PageAnchor& outAnchor, std::string& errOut) const;
    void toggleSelectCurrentLine();
    bool lineSelected(uint64_t lineNumber) const;
    int hitTestRow(int y) const;
    const LineView* currentLine() const;
    fs::FileProvider* resolveWritableProvider(std::string& relPath, std::string& errOut) const;
    bool copySelectionToClipboard(std::string& errOut);
    bool cutSelection(std::string& errOut);
    bool pasteBeforeCurrentLine(std::string& errOut);
    bool pasteAfterCurrentLine(std::string& errOut);
    bool insertEmptyLineBefore(std::string& errOut);
    bool insertEmptyLineAfter(std::string& errOut);
    bool readRangeToString(uint64_t offset, uint64_t size, std::string& out,
                           std::string& errOut) const;
    bool rewriteFile(const std::vector<RewriteSegment>& segments, uint64_t anchorByte,
                     uint64_t anchorLineNumber, int preferredCursor, std::string& errOut);
    bool writeSegmentsToTempPath(const std::string& tempRelPath,
                                 const std::vector<RewriteSegment>& segments,
                                 std::string& errOut);

    fs::ProviderManager* provMgr_ = nullptr;
    std::string          path_;
    std::string          displayPath_;
    bool                 active_ = false;
    bool                 readOnly_ = false;
    uint64_t             fileSize_ = 0;
    uint64_t             contentStartByte_ = 0;
    bool                 fileEndsWithNewline_ = false;
    std::vector<LineView> pageLines_;
    std::vector<PageAnchor> pageHistory_;
    int                  cursorIndex_ = 0;
    uint64_t             pageStartByte_ = 0;
    uint64_t             pageStartLineNumber_ = 0;
    SelectionState       selection_;
    std::string          textClipboard_;
    std::string          pendingError_;
};

} // namespace xxplore
