#pragma once
#include "ui/modals.hpp"
#include "ui/touch_event.hpp"
#include "util/websocket_installer_server.hpp"
#include <cstdint>

namespace xxplore {

class Renderer;
class FontManager;
class I18n;

enum class WebSocketInstallerAction { None, Close };

class WebSocketInstallerScreen {
public:
    ~WebSocketInstallerScreen() { close(); }

    void open(const I18n& i18n);
    void close();

    bool isOpen() const { return open_; }
    WebSocketInstallerAction handleInput(uint64_t kDown, const TouchTap* tap = nullptr);
    void render(Renderer& renderer, FontManager& fm, const I18n& i18n) const;

private:
    int focusRow_ = 1; // 0 target row, 1 button row
    int targetFocusCol_ = 1;
    int buttonFocusCol_ = 1; // 0 cancel, 1 start
    bool open_ = false;
    std::string interruptButtonLabel_;
    std::string interruptConfirmTitle_;
    std::string interruptConfirmBody_;
    ModalConfirm interruptConfirm_;
    WebSocketInstallerServer server_;
};

} // namespace xxplore
