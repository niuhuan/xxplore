#include "ui/modals.hpp"
#include "ui/renderer.hpp"
#include "ui/font_manager.hpp"
#include "ui/theme.hpp"
#include "i18n/i18n.hpp"
#include <switch.h>

namespace xplore {

static constexpr int kCardW = 560;
static constexpr int kCardH = 220;
static constexpr int kPad   = 20;

void ModalConfirm::open(std::string t, std::string b) {
    active  = true;
    title   = std::move(t);
    body    = std::move(b);
    focusOk = 1;
}

void ModalConfirm::close() {
    active = false;
    title.clear();
    body.clear();
}

ConfirmResult ModalConfirm::handleInput(uint64_t kDown) {
    if (!active) return ConfirmResult::None;
    if (kDown & HidNpadButton_A)
        return focusOk ? ConfirmResult::Confirmed : ConfirmResult::Cancelled;
    if (kDown & HidNpadButton_B)
        return ConfirmResult::Cancelled;
    if (kDown & (HidNpadButton_AnyLeft | HidNpadButton_AnyRight))
        focusOk = !focusOk;
    return ConfirmResult::None;
}

void ModalConfirm::render(Renderer& renderer, FontManager& fm, const I18n& i18n) {
    if (!active) return;
    using namespace theme;

    int cx = (SCREEN_W - kCardW) / 2;
    int cy = (SCREEN_H - kCardH) / 2;

    renderer.drawRoundedRectFilled(cx, cy, kCardW, kCardH, MENU_RADIUS, MENU_BG);
    renderer.drawRoundedRect(cx, cy, kCardW, kCardH, MENU_RADIUS, MENU_BORDER);

    int tx = cx + kPad;
    int ty = cy + kPad;
    fm.drawText(renderer.sdl(), title.c_str(), tx, ty, FONT_SIZE_ITEM, TEXT);
    ty += FONT_SIZE_ITEM + 12;
    fm.drawTextEllipsis(renderer.sdl(), body.c_str(), tx, ty, FONT_SIZE_SMALL, TEXT_SECONDARY,
                        kCardW - kPad * 2);

    const char* okTxt    = i18n.t("modal.ok");
    const char* cancelTxt = i18n.t("modal.cancel");
    int btnY             = cy + kCardH - 56;
    int okW              = fm.measureText(okTxt, FONT_SIZE_ITEM);
    int cancelW          = fm.measureText(cancelTxt, FONT_SIZE_ITEM);
    int gap              = 40;
    int total            = okW + gap + cancelW;
    int bx               = cx + (kCardW - total) / 2;

    SDL_Color okCol    = focusOk ? PRIMARY : TEXT_SECONDARY;
    SDL_Color canCol   = focusOk ? TEXT_SECONDARY : PRIMARY;
    fm.drawText(renderer.sdl(), okTxt, bx, btnY, FONT_SIZE_ITEM, okCol);
    fm.drawText(renderer.sdl(), cancelTxt, bx + okW + gap, btnY, FONT_SIZE_ITEM, canCol);
}

void ModalProgress::open(std::string t, std::string d) {
    active = true;
    title  = std::move(t);
    detail = std::move(d);
}

void ModalProgress::close() {
    active = false;
    title.clear();
    detail.clear();
}

void ModalProgress::render(Renderer& renderer, FontManager& fm) {
    if (!active) return;
    using namespace theme;

    int cx = (SCREEN_W - kCardW) / 2;
    int cy = (SCREEN_H - 160) / 2;
    int h  = 140;

    renderer.drawRoundedRectFilled(cx, cy, kCardW, h, MENU_RADIUS, MENU_BG);
    renderer.drawRoundedRect(cx, cy, kCardW, h, MENU_RADIUS, MENU_BORDER);

    int tx = cx + kPad;
    int ty = cy + kPad;
    fm.drawText(renderer.sdl(), title.c_str(), tx, ty, FONT_SIZE_ITEM, TEXT);
    ty += FONT_SIZE_ITEM + 10;
    fm.drawTextEllipsis(renderer.sdl(), detail.c_str(), tx, ty, FONT_SIZE_SMALL, TEXT_SECONDARY,
                        kCardW - kPad * 2);
}

void ModalInfo::open(std::string t, std::string b) {
    active = true;
    title  = std::move(t);
    body   = std::move(b);
}

void ModalInfo::close() {
    active = false;
    title.clear();
    body.clear();
}

ConfirmResult ModalInfo::handleInput(uint64_t kDown) {
    if (!active) return ConfirmResult::None;
    if (kDown & (HidNpadButton_A | HidNpadButton_B))
        return ConfirmResult::Confirmed;
    return ConfirmResult::None;
}

void ModalInfo::render(Renderer& renderer, FontManager& fm) {
    if (!active) return;
    using namespace theme;

    int cardH = 420;
    int cx    = (SCREEN_W - kCardW) / 2;
    int cy    = (SCREEN_H - cardH) / 2;

    renderer.drawRoundedRectFilled(cx, cy, kCardW, cardH, MENU_RADIUS, MENU_BG);
    renderer.drawRoundedRect(cx, cy, kCardW, cardH, MENU_RADIUS, MENU_BORDER);

    int tx = cx + kPad;
    int ty = cy + kPad;
    fm.drawText(renderer.sdl(), title.c_str(), tx, ty, FONT_SIZE_ITEM, TEXT);
    ty += FONT_SIZE_ITEM + 12;

    const char* p = body.c_str();
    int lineH = fm.fontHeight(FONT_SIZE_SMALL) + 4;
    int maxY = cy + cardH - kPad;
    while (*p && ty < maxY) {
        const char* nl = strchr(p, '\n');
        std::string line(p, nl ? nl : p + strlen(p));
        fm.drawTextEllipsis(renderer.sdl(), line.c_str(), tx, ty, FONT_SIZE_SMALL,
                            TEXT_SECONDARY, kCardW - kPad * 2);
        ty += lineH;
        if (!nl) break;
        p = nl + 1;
    }
}

} // namespace xplore
