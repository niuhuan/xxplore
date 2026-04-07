#pragma once
#include <SDL.h>
#include <string>

namespace xplore {

class Renderer;
class FontManager;
class I18n;

enum class ConfirmResult { None, Confirmed, Cancelled };

/// Two-button A/B confirm dialog (delete, overwrite, …).
class ModalConfirm {
public:
    void open(std::string title, std::string body);
    void close();

    bool isOpen() const { return active; }

    /// @param kDown  padGetButtonsDown this frame
    ConfirmResult handleInput(uint64_t kDown);

    void render(Renderer& renderer, FontManager& fm, const I18n& i18n);

private:
    bool        active = false;
    std::string title;
    std::string body;
    int         focusOk = 1; ///< 1 = OK focused, 0 = Cancel focused
};

/// Blocking-style progress (copy/move); no dismiss until host closes.
class ModalProgress {
public:
    void open(std::string title, std::string detail);
    void setDetail(std::string d) { detail = std::move(d); }
    void close();

    bool isOpen() const { return active; }
    void render(Renderer& renderer, FontManager& fm);

private:
    bool        active = false;
    std::string title;
    std::string detail;
};

/// Scroll-free multi-line info (help / about / clipboard list).
class ModalInfo {
public:
    void open(std::string title, std::string body);
    void close();

    bool isOpen() const { return active; }

    ConfirmResult handleInput(uint64_t kDown);
    void render(Renderer& renderer, FontManager& fm);

private:
    bool        active = false;
    std::string title;
    std::string body;
};

} // namespace xplore
