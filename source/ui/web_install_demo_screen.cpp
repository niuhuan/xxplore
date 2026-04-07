#include "ui/web_install_demo_screen.hpp"
#include "ui/font_manager.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include <algorithm>
#include <string>
#include <vector>
#include <switch.h>

namespace xplore {

void WebInstallDemoScreen::open() {
    close();
    open_ = true;
    server_.start();
}

void WebInstallDemoScreen::close() {
    if (!open_)
        return;
    server_.stop();
    open_ = false;
}

WebInstallDemoAction WebInstallDemoScreen::handleInput(uint64_t kDown) {
    if (!open_)
        return WebInstallDemoAction::None;
    if (kDown & (HidNpadButton_B | HidNpadButton_Plus))
        return WebInstallDemoAction::Close;
    return WebInstallDemoAction::None;
}

void WebInstallDemoScreen::render(Renderer& renderer, FontManager& fm) const {
    if (!open_)
        return;

    renderer.drawRectFilled(0, 0, theme::SCREEN_W, theme::SCREEN_H, theme::BG);

    const int cardX = 40;
    const int cardY = 50;
    const int cardW = theme::SCREEN_W - 80;
    const int cardH = theme::SCREEN_H - 100;

    renderer.drawRoundedRectFilled(cardX, cardY, cardW, cardH, 16, theme::MENU_BG);
    renderer.drawRoundedRect(cardX, cardY, cardW, cardH, 16, theme::MENU_BORDER);

    const int x = cardX + 24;
    int y = cardY + 20;

    fm.drawText(renderer.sdl(), "Web Install Demo", x, y, theme::FONT_SIZE_TITLE, theme::PRIMARY);
    y += 32;
    fm.drawText(renderer.sdl(), "XPLORE_DEBUG only. Open the URL on desktop, then click Ping.",
                x, y, theme::FONT_SIZE_SMALL, theme::TEXT_SECONDARY);
    y += 28;
    fm.drawText(renderer.sdl(), "B/+: close", x, y, theme::FONT_SIZE_SMALL, theme::TEXT_SECONDARY);
    y += 34;

    renderer.drawRoundedRectFilled(x, y, cardW - 48, 68, 12, theme::SURFACE);
    renderer.drawRoundedRect(x, y, cardW - 48, 68, 12, theme::DIVIDER);
    fm.drawText(renderer.sdl(), "URL", x + 16, y + 12, theme::FONT_SIZE_SMALL, theme::TEXT_SECONDARY);
    std::string url = server_.url();
    fm.drawTextEllipsis(renderer.sdl(), url.c_str(), x + 16, y + 34, theme::FONT_SIZE_ITEM,
                        theme::TEXT, cardW - 80);
    y += 84;

    renderer.drawRoundedRectFilled(x, y, cardW - 48, 56, 12, theme::SURFACE);
    renderer.drawRoundedRect(x, y, cardW - 48, 56, 12, theme::DIVIDER);
    fm.drawText(renderer.sdl(), "Status", x + 16, y + 10, theme::FONT_SIZE_SMALL, theme::TEXT_SECONDARY);
    std::string status = server_.status();
    fm.drawTextEllipsis(renderer.sdl(), status.c_str(), x + 16, y + 28, theme::FONT_SIZE_SMALL,
                        theme::TEXT, cardW - 80);
    y += 72;

    const int logH = cardY + cardH - y - 24;
    renderer.drawRoundedRectFilled(x, y, cardW - 48, logH, 12, theme::BG);
    renderer.drawRoundedRect(x, y, cardW - 48, logH, 12, theme::DIVIDER);
    fm.drawText(renderer.sdl(), "Server Log", x + 12, y + 10, theme::FONT_SIZE_SMALL,
                theme::TEXT_SECONDARY);

    std::vector<std::string> logs = server_.logs();
    int lineY = y + 34;
    int lineH = fm.fontHeight(theme::FONT_SIZE_SMALL) + 4;
    int maxVisible = std::max(1, (logH - 46) / lineH);
    int start = 0;
    if (static_cast<int>(logs.size()) > maxVisible)
        start = static_cast<int>(logs.size()) - maxVisible;
    for (int i = start; i < static_cast<int>(logs.size()); ++i) {
        fm.drawTextEllipsis(renderer.sdl(), logs[static_cast<size_t>(i)].c_str(), x + 12, lineY,
                            theme::FONT_SIZE_SMALL, theme::TEXT, cardW - 72);
        lineY += lineH;
    }
}

} // namespace xplore
