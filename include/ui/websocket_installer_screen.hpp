#pragma once
#include "ui/touch_event.hpp"
#include "util/websocket_installer_server.hpp"
#include <cstdint>

namespace xplore {

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
    WebSocketInstallerServer server_;
};

} // namespace xplore
