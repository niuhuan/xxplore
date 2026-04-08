#include "ui/panel_chrome.hpp"

#include "ui/font_manager.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"

namespace xplore::ui {

namespace {

void drawCloseGlyph(Renderer& renderer, int x, int y, int size, SDL_Color color) {
    const int pad = 10;
    renderer.drawLine(x + pad, y + pad, x + size - pad, y + size - pad, color);
    renderer.drawLine(x + size - pad, y + pad, x + pad, y + size - pad, color);
}

}

void drawPanelTitleBar(Renderer& renderer, FontManager& fm, int x, int y, int w,
                       const char* title, bool showCloseButton, bool closeFocused) {
    renderer.drawRoundedRectFilled(x, y, w, kPanelTitleBarH, 12, theme::HEADER_BG);
    renderer.drawLine(x + 12, y + kPanelTitleBarH, x + w - 12, y + kPanelTitleBarH,
                      theme::DIVIDER);

    const int textY = y + (kPanelTitleBarH - kPanelTitleFontSize) / 2 - 1;
    fm.drawText(renderer.sdl(), title, x + 18, textY, kPanelTitleFontSize, theme::PRIMARY);

    if (!showCloseButton)
        return;

    const int btnX = x + w - 14 - kPanelCloseButtonSize;
    const int btnY = y + (kPanelTitleBarH - kPanelCloseButtonSize) / 2;
    SDL_Color bg = closeFocused ? theme::PRIMARY_DIM : theme::SURFACE;
    SDL_Color border = closeFocused ? theme::PRIMARY : theme::DIVIDER;
    SDL_Color glyph = closeFocused ? theme::ON_PRIMARY : theme::TEXT;
    renderer.drawRoundedRectFilled(btnX, btnY, kPanelCloseButtonSize, kPanelCloseButtonSize, 10, bg);
    renderer.drawRoundedRect(btnX, btnY, kPanelCloseButtonSize, kPanelCloseButtonSize, 10, border);
    drawCloseGlyph(renderer, btnX, btnY, kPanelCloseButtonSize, glyph);
}

bool panelCloseButtonHit(int cardX, int cardY, int cardW, int tapX, int tapY) {
    const int btnX = cardX + cardW - 14 - kPanelCloseButtonSize;
    const int btnY = cardY + (kPanelTitleBarH - kPanelCloseButtonSize) / 2;
    return tapX >= btnX && tapX < btnX + kPanelCloseButtonSize &&
           tapY >= btnY && tapY < btnY + kPanelCloseButtonSize;
}

} // namespace xplore::ui
