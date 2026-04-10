#include "ui/ftp_server_screen.hpp"

#include "i18n/i18n.hpp"
#include "ui/font_manager.hpp"
#include "ui/panel_chrome.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "util/screen_awake.hpp"

#include <algorithm>
#include <string>
#include <switch.h>
#include <vector>

namespace xxplore {

void FtpServerScreen::open() {
    close();
    util::acquireScreenAwake();
    open_ = true;
    server_.start();
}

void FtpServerScreen::close() {
    if (!open_)
        return;
    server_.stop();
    util::releaseScreenAwake();
    open_ = false;
}

FtpServerScreenAction FtpServerScreen::handleInput(uint64_t kDown, const TouchTap* tap) {
    if (!open_)
        return FtpServerScreenAction::None;

    const int cardX = 40;
    const int cardY = 40;
    const int cardW = theme::SCREEN_W - 80;
    if ((tap && tap->active && ui::panelCloseButtonHit(cardX, cardY, cardW, tap->x, tap->y)) ||
        (kDown & (HidNpadButton_B | HidNpadButton_Plus))) {
        return FtpServerScreenAction::Close;
    }
    return FtpServerScreenAction::None;
}

void FtpServerScreen::render(Renderer& renderer, FontManager& fm, const I18n& i18n) const {
    if (!open_)
        return;

    renderer.drawRectFilled(0, 0, theme::SCREEN_W, theme::SCREEN_H, theme::BG);

    const int cardX = 40;
    const int cardY = 40;
    const int cardW = theme::SCREEN_W - 80;
    const int cardH = theme::SCREEN_H - 80;

    renderer.drawRoundedRectFilled(cardX, cardY, cardW, cardH, 16, theme::MENU_BG);
    renderer.drawRoundedRect(cardX, cardY, cardW, cardH, 16, theme::MENU_BORDER);
    ui::drawPanelTitleBar(renderer, fm, cardX, cardY, cardW, i18n.t("ftp_server.title"), true, false);

    const int x = cardX + 24;
    const int contentW = cardW - 48;
    const int contentRight = x + contentW;
    int y = cardY + ui::kPanelTitleBarH + 18;

    fm.drawText(renderer.sdl(), i18n.t("ftp_server.description"), x, y, theme::FONT_SIZE_SMALL,
                theme::TEXT_SECONDARY);
    y += 28;

    std::string url = server_.url();
    if (url.empty())
        url = i18n.t("ftp_server.start_failed");
    fm.drawTextEllipsis(renderer.sdl(), url.c_str(), x, y, theme::FONT_SIZE_ITEM, theme::TEXT,
                        contentW);
    y += 34;

    std::string statusLine = std::string(i18n.t("ftp_server.status_label")) + ": " + server_.status();
    fm.drawTextEllipsis(renderer.sdl(), statusLine.c_str(), x, y, theme::FONT_SIZE_SMALL,
                        theme::TEXT_SECONDARY, contentW);
    y += 22;

    std::string sessionLine = std::string(i18n.t("ftp_server.client_count")) + ": " +
                              std::to_string(server_.sessionCount());
    fm.drawText(renderer.sdl(), sessionLine.c_str(), x, y, theme::FONT_SIZE_SMALL,
                theme::TEXT_SECONDARY);

    const char* rootHint = i18n.t("ftp_server.root_hint");
    int rootW = fm.measureText(rootHint, theme::FONT_SIZE_SMALL);
    fm.drawText(renderer.sdl(), rootHint, contentRight - rootW, y, theme::FONT_SIZE_SMALL,
                theme::TEXT_SECONDARY);
    y += 30;

    const int logY = y;
    const int logH = cardY + cardH - 34 - logY;
    renderer.drawRoundedRectFilled(x, logY, contentW, logH, 10, theme::BG);
    renderer.drawRoundedRect(x, logY, contentW, logH, 10, theme::DIVIDER);
    fm.drawText(renderer.sdl(), i18n.t("installer.console"), x + 12, logY + 10,
                theme::FONT_SIZE_SMALL, theme::TEXT_SECONDARY);

    std::vector<std::string> logs = server_.logs();
    int lineY = logY + 36;
    int lineH = fm.fontHeight(theme::FONT_SIZE_SMALL) + 4;
    int maxVisible = std::max(1, (logH - 48) / lineH);
    int start = 0;
    if (static_cast<int>(logs.size()) > maxVisible)
        start = static_cast<int>(logs.size()) - maxVisible;
    for (int i = start; i < static_cast<int>(logs.size()); ++i) {
        fm.drawTextEllipsis(renderer.sdl(), logs[static_cast<std::size_t>(i)].c_str(), x + 12,
                            lineY, theme::FONT_SIZE_SMALL, theme::TEXT, contentW - 24);
        lineY += lineH;
    }
}

} // namespace xxplore
