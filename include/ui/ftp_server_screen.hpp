#pragma once

#include "ui/touch_event.hpp"
#include "util/ftp_server.hpp"
#include <cstdint>

namespace xxplore {

class Renderer;
class FontManager;
class I18n;

enum class FtpServerScreenAction { None, Close };

class FtpServerScreen {
public:
    ~FtpServerScreen() { close(); }

    void open();
    void close();

    bool isOpen() const { return open_; }
    FtpServerScreenAction handleInput(uint64_t kDown, const TouchTap* tap = nullptr);
    void render(Renderer& renderer, FontManager& fm, const I18n& i18n) const;

private:
    bool open_ = false;
    FtpServer server_;
};

} // namespace xxplore
