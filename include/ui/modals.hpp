#pragma once
#include <SDL.h>
#include <string>

namespace xplore {

class Renderer;
class FontManager;
class I18n;

enum class ConfirmResult { None, Confirmed, Cancelled };
enum class ChoiceResult  { None, Cancel, Merge, Overwrite };
enum class InstallPromptResult { None, Cancel, Install, InstallAndDelete };

/// Two-button OK/Cancel confirm dialog (delete, …). B = Cancel.
class ModalConfirm {
public:
    void open(std::string title, std::string body);
    void close();

    bool isOpen() const { return active; }
    ConfirmResult handleInput(uint64_t kDown);
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
    ChoiceResult handleInput(uint64_t kDown);
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
    void close();

    bool isOpen() const { return active; }
    void render(Renderer& renderer, FontManager& fm, const I18n& i18n);

private:
    bool        active = false;
    std::string title;
    std::string detail;
};

/// Scroll-free multi-line info (help / about / clipboard list). B = close.
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

/// Three-button install prompt: Cancel / Install / Install+Delete.
class ModalInstallPrompt {
public:
    void open(std::string title, std::string body);
    void close();

    bool isOpen() const { return active; }
    InstallPromptResult handleInput(uint64_t kDown);
    void render(Renderer& renderer, FontManager& fm, const I18n& i18n);

private:
    bool        active = false;
    std::string title;
    std::string body;
    int         focus = 0; ///< 0=Cancel, 1=Install, 2=Install+Delete
};

} // namespace xplore
