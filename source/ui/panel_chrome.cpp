#include "ui/panel_chrome.hpp"

#include "ui/font_manager.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"

namespace xxplore::ui {

namespace {

void drawCloseGlyph(Renderer& renderer, int x, int y, int size, SDL_Color color) {
    const int pad = 10;
    renderer.drawLine(x + pad, y + pad, x + size - pad, y + size - pad, color);
    renderer.drawLine(x + size - pad, y + pad, x + pad, y + size - pad, color);
}

}

void drawPanelCloseButton(Renderer& renderer, int cardX, int cardY, int cardW, bool closeFocused) {
    const int btnX = cardX + cardW - 14 - kPanelCloseButtonSize;
    const int btnY = cardY + (kPanelTitleBarH - kPanelCloseButtonSize) / 2;
    SDL_Color bg = closeFocused ? theme::PRIMARY_DIM : theme::SURFACE;
    SDL_Color border = closeFocused ? theme::PRIMARY : theme::DIVIDER;
    SDL_Color glyph = closeFocused ? theme::ON_PRIMARY : theme::TEXT;
    renderer.drawRoundedRectFilled(btnX, btnY, kPanelCloseButtonSize, kPanelCloseButtonSize, 10, bg);
    renderer.drawRoundedRect(btnX, btnY, kPanelCloseButtonSize, kPanelCloseButtonSize, 10, border);
    drawCloseGlyph(renderer, btnX, btnY, kPanelCloseButtonSize, glyph);
}

void drawPanelTextButton(Renderer& renderer, FontManager& fm, int cardX, int cardY, int cardW,
                         int buttonW, const char* label, bool focused) {
    const int btnX = cardX + cardW - 14 - buttonW;
    const int btnY = cardY + (kPanelTitleBarH - kPanelCloseButtonSize) / 2;
    SDL_Color bg = focused ? theme::PRIMARY_DIM : theme::SURFACE;
    SDL_Color border = focused ? theme::PRIMARY : theme::DIVIDER;
    SDL_Color text = focused ? theme::ON_PRIMARY : theme::TEXT;
    renderer.drawRoundedRectFilled(btnX, btnY, buttonW, kPanelCloseButtonSize, 10, bg);
    renderer.drawRoundedRect(btnX, btnY, buttonW, kPanelCloseButtonSize, 10, border);
    int textW = fm.measureText(label, theme::FONT_SIZE_SMALL);
    int textH = fm.fontHeight(theme::FONT_SIZE_SMALL);
    int textX = btnX + (buttonW - textW) / 2;
    int textY = btnY + (kPanelCloseButtonSize - textH) / 2;
    fm.drawText(renderer.sdl(), label, textX, textY, theme::FONT_SIZE_SMALL, text);
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

    drawPanelCloseButton(renderer, x, y, w, closeFocused);
}

bool panelCloseButtonHit(int cardX, int cardY, int cardW, int tapX, int tapY) {
    const int btnX = cardX + cardW - 14 - kPanelCloseButtonSize;
    const int btnY = cardY + (kPanelTitleBarH - kPanelCloseButtonSize) / 2;
    return tapX >= btnX && tapX < btnX + kPanelCloseButtonSize &&
           tapY >= btnY && tapY < btnY + kPanelCloseButtonSize;
}

bool panelTextButtonHit(int cardX, int cardY, int cardW, int buttonW, int tapX, int tapY) {
    const int btnX = cardX + cardW - 14 - buttonW;
    const int btnY = cardY + (kPanelTitleBarH - kPanelCloseButtonSize) / 2;
    return tapX >= btnX && tapX < btnX + buttonW &&
           tapY >= btnY && tapY < btnY + kPanelCloseButtonSize;
}

} // namespace xxplore::ui
