#include "ui/toast.hpp"
#include "ui/renderer.hpp"
#include "ui/font_manager.hpp"
#include "ui/theme.hpp"

namespace xplore {

void Toast::show(const std::string& t, const std::string& d, uint32_t durationMs) {
    title = t;
    detail = d;
    remainingMs = durationMs;
}

void Toast::update(uint32_t deltaMs) {
    if (remainingMs > deltaMs)
        remainingMs -= deltaMs;
    else
        remainingMs = 0;
}

void Toast::render(Renderer& renderer, FontManager& fontManager) {
    if (!isVisible()) return;

    int x = (theme::SCREEN_W - theme::TOAST_W) / 2;
    int y = theme::SCREEN_H - theme::TOAST_H - theme::TOAST_MARGIN;

    renderer.drawRectFilled(x, y, theme::TOAST_W, theme::TOAST_H, theme::TOAST_BG);
    renderer.drawRectFilled(x, y, theme::TOAST_BORDER_W, theme::TOAST_H, theme::TOAST_BORDER);

    int textX = x + theme::PADDING + theme::TOAST_BORDER_W;
    fontManager.drawText(renderer.sdl(), title.c_str(),
        textX, y + 12, theme::FONT_SIZE_SMALL, theme::TOAST_TEXT);
    fontManager.drawText(renderer.sdl(), detail.c_str(),
        textX, y + 12 + theme::FONT_SIZE_SMALL + 10, theme::FONT_SIZE_SMALL, theme::TOAST_TEXT);
}

} // namespace xplore
