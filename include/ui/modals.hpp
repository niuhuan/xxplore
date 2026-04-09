#pragma once
#include "ui/touch_event.hpp"
#include <SDL.h>
#include <cstdint>
#include <string>
#include <vector>

namespace xplore {

class Renderer;
class FontManager;
class I18n;

enum class ConfirmResult { None, Confirmed, Cancelled };
enum class ChoiceResult  { None, Cancel, Merge, Overwrite };
enum class InstallPromptResult { None, Cancel, Install, InstallAndDelete };
enum class ErrorActionResult { None, Abort, Ignore, IgnoreAll };

/// Two-button OK/Cancel confirm dialog (delete, …). B = Cancel.
class ModalConfirm {
public:
    void open(std::string title, std::string body);
    void close();

    bool isOpen() const { return active; }
    ConfirmResult handleInput(uint64_t kDown, const TouchTap* tap = nullptr);
    void render(Renderer& renderer, FontManager& fm, const I18n& i18n);

private:
    bool        active = false;
    std::string title;
    std::string body;
    int         focusOk = 1;
};

/// Three-button choice dialog (Cancel / Merge / Overwrite). B = Cancel.
class ModalChoice {
public:
    void open(std::string title, std::string body);
    void close();

    bool isOpen() const { return active; }
    ChoiceResult handleInput(uint64_t kDown, const TouchTap* tap = nullptr);
    void render(Renderer& renderer, FontManager& fm, const I18n& i18n);

private:
    bool        active = false;
    std::string title;
    std::string body;
    int         focus = 0; ///< 0=Cancel, 1=Merge, 2=Overwrite
};

/// Blocking-style progress; shows "- to interrupt" hint.
class ModalProgress {
public:
    void open(std::string title, std::string detail);
    void setDetail(std::string d) { detail = std::move(d); }
    void setProgress(uint64_t current, uint64_t total) {
        progressCurrent = current;
        progressTotal = total;
    }
    void close();

    bool isOpen() const { return active; }
    void render(Renderer& renderer, FontManager& fm, const I18n& i18n);

private:
    bool        active = false;
    std::string title;
    std::string detail;
    uint64_t    progressCurrent = 0;
    uint64_t    progressTotal = 0;
};

/// Three-button error action dialog: Abort / Ignore / Ignore All.
class ModalErrorAction {
public:
    void open(std::string title, std::string body, std::string detail);
    void close();

    bool isOpen() const { return active; }
    ErrorActionResult handleInput(uint64_t kDown, const TouchTap* tap = nullptr);
    void render(Renderer& renderer, FontManager& fm, const I18n& i18n);

private:
    bool        active = false;
    std::string title;
    std::string body;
    std::string detail;
    int         focus = 0; ///< 0=Abort, 1=Ignore, 2=IgnoreAll
};

/// Scroll-free multi-line info (help / about / clipboard list). B = close.
class ModalInfo {
public:
    void open(std::string title, std::string body, int bodyFontSize = 0);
    void close();

    bool isOpen() const { return active; }
    ConfirmResult handleInput(uint64_t kDown, const TouchTap* tap = nullptr);
    void render(Renderer& renderer, FontManager& fm);

private:
    bool        active = false;
    std::string title;
    std::string body;
    int         bodyFontSize = 0;
    std::vector<std::string> lines;
    int         pageIndex = 0;
};

/// Three-button install prompt: Cancel / Install / Install+Delete.
class ModalInstallPrompt {
public:
    void open(std::string title, std::string body);
    void close();

    bool isOpen() const { return active; }
    InstallPromptResult handleInput(uint64_t kDown, const TouchTap* tap = nullptr);
    void render(Renderer& renderer, FontManager& fm, const I18n& i18n);

private:
    bool        active = false;
    std::string title;
    std::string body;
    int         focus = 0; ///< 0=Cancel, 1=Install, 2=Install+Delete
};

} // namespace xplore
