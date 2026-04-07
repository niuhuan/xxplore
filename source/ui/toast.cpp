#include "ui/toast.hpp"
#include "ui/renderer.hpp"
#include "ui/font_manager.hpp"
#include "ui/theme.hpp"

namespace xplore {

namespace {

struct ToastPalette {
    SDL_Color border;
};

static ToastPalette paletteFor(ToastKind kind) {
    using namespace theme;
    switch (kind) {
        case ToastKind::Success:
            return {TOAST_SUCCESS_BORDER};
        case ToastKind::Warning:
            return {TOAST_WARNING_BORDER};
        case ToastKind::Info:
            return {TOAST_INFO_BORDER};
        case ToastKind::Error:
        default:
            return {TOAST_ERROR_BORDER};
    }
}

} // namespace

void Toast::show(const std::string& t, const std::string& d, ToastKind k, uint32_t durationMs) {
    title = t;
    detail = d;
    kind = k;
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

    ToastPalette palette = paletteFor(kind);

    int x = (theme::SCREEN_W - theme::TOAST_W) / 2;
    int y = theme::SCREEN_H - theme::TOAST_H - theme::TOAST_MARGIN;

    renderer.drawRectFilled(x, y, theme::TOAST_W, theme::TOAST_H, theme::TOAST_BG);
    renderer.drawRectFilled(x, y, theme::TOAST_BORDER_W, theme::TOAST_H, palette.border);

    int textX = x + theme::PADDING + theme::TOAST_BORDER_W;
    if (detail.empty()) {
        fontManager.drawText(renderer.sdl(), title.c_str(),
            textX, y + (theme::TOAST_H - theme::FONT_SIZE_SMALL) / 2,
            theme::FONT_SIZE_SMALL, theme::TOAST_TEXT);
    } else {
        fontManager.drawText(renderer.sdl(), title.c_str(),
            textX, y + 12, theme::FONT_SIZE_SMALL, theme::TOAST_TEXT);
        fontManager.drawText(renderer.sdl(), detail.c_str(),
            textX, y + 12 + theme::FONT_SIZE_SMALL + 10,
            theme::FONT_SIZE_SMALL, theme::TOAST_TEXT);
    }
}

} // namespace xplore
