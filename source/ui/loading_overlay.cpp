#include "ui/loading_overlay.hpp"
#include "ui/renderer.hpp"
#include "ui/font_manager.hpp"
#include "ui/theme.hpp"
#include "i18n/i18n.hpp"

namespace xxplore {

void LoadingOverlay::show(std::string message, uint32_t timeoutMs, uint32_t delayMs) {
    active_    = true;
    visible_   = false;
    timedOut_  = false;
    message_   = std::move(message);
    delayMs_   = delayMs;
    timeoutMs_ = timeoutMs;
    elapsed_   = 0;
    dotTimer_  = 0;
    dotCount_  = 0;
}

void LoadingOverlay::hide() {
    active_  = false;
    visible_ = false;
}

void LoadingOverlay::update(uint32_t deltaMs) {
    if (!active_ || timedOut_) return;

    elapsed_ += deltaMs;

    if (!visible_ && elapsed_ >= delayMs_)
        visible_ = true;

    if (timeoutMs_ > 0 && elapsed_ >= timeoutMs_)
        timedOut_ = true;

    // Animate dots
    dotTimer_ += deltaMs;
    if (dotTimer_ >= 400) {
        dotTimer_ -= 400;
        dotCount_ = (dotCount_ + 1) % 4;
    }
}

void LoadingOverlay::render(Renderer& renderer, FontManager& fm, const I18n& /*i18n*/) {
    if (!active_ || !visible_) return;
    using namespace theme;

    // Full screen scrim
    renderer.drawRectFilled(0, 0, SCREEN_W, SCREEN_H, MENU_OVERLAY);

    // Centered card
    constexpr int cardW = 420;
    constexpr int cardH = 120;
    int cx = (SCREEN_W - cardW) / 2;
    int cy = (SCREEN_H - cardH) / 2;

    renderer.drawRoundedRectFilled(cx, cy, cardW, cardH, MENU_RADIUS, MENU_BG);
    renderer.drawRoundedRect(cx, cy, cardW, cardH, MENU_RADIUS, MENU_BORDER);

    // Message with animated dots
    static const char* kDots[] = {"", ".", "..", "..."};
    std::string display = message_ + kDots[dotCount_];

    int textY = cy + (cardH - FONT_SIZE_ITEM) / 2;
    int textMaxW = cardW - PADDING * 2;
    fm.drawTextEllipsis(renderer.sdl(), display.c_str(), cx + PADDING, textY,
                        FONT_SIZE_ITEM, TEXT, textMaxW);
}

} // namespace xxplore
