#include "ui/modals.hpp"
#include "ui/renderer.hpp"
#include "ui/font_manager.hpp"
#include "ui/panel_chrome.hpp"
#include "ui/theme.hpp"
#include "i18n/i18n.hpp"
#include <switch.h>

namespace xplore {

static constexpr int kCardW = 560;
static constexpr int kCardH = 220;
static constexpr int kPad   = 20;

// ---- ModalConfirm (OK / Cancel) ----

void ModalConfirm::open(std::string t, std::string b) {
    active  = true;
    title   = std::move(t);
    body    = std::move(b);
    focusOk = 0;
}

void ModalConfirm::close() {
    active = false;
    title.clear();
    body.clear();
}

ConfirmResult ModalConfirm::handleInput(uint64_t kDown, const TouchTap* tap) {
    if (!active) return ConfirmResult::None;
    int cx = (theme::SCREEN_W - kCardW) / 2;
    int cy = (theme::SCREEN_H - kCardH) / 2;
    int btnY = cy + kCardH - 56;
    int cancelW = 120;
    int okW = 120;
    int gap = 40;
    int total = cancelW + gap + okW;
    int bx = cx + (kCardW - total) / 2;
    if (tap && tap->active) {
        if (pointInRect(tap, bx - 12, btnY - 8, cancelW, 36)) {
            focusOk = 0;
            return ConfirmResult::Cancelled;
        }
        if (pointInRect(tap, bx + cancelW + gap - 12, btnY - 8, okW, 36)) {
            focusOk = 1;
            return ConfirmResult::Confirmed;
        }
    }
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
    int total            = cancelW + gap + okW;
    int bx               = cx + (kCardW - total) / 2;

    SDL_Color canCol   = (focusOk == 0) ? PRIMARY : TEXT_SECONDARY;
    SDL_Color okCol    = (focusOk == 1) ? PRIMARY : TEXT_SECONDARY;
    fm.drawText(renderer.sdl(), cancelTxt, bx, btnY, FONT_SIZE_ITEM, canCol);
    fm.drawText(renderer.sdl(), okTxt, bx + cancelW + gap, btnY, FONT_SIZE_ITEM, okCol);
}

// ---- ModalChoice (Cancel / Merge / Overwrite) ----

void ModalChoice::open(std::string t, std::string b) {
    active = true;
    title  = std::move(t);
    body   = std::move(b);
    focus  = 0;
}

void ModalChoice::close() {
    active = false;
    title.clear();
    body.clear();
}

ChoiceResult ModalChoice::handleInput(uint64_t kDown, const TouchTap* tap) {
    if (!active) return ChoiceResult::None;
    int cx = (theme::SCREEN_W - kCardW) / 2;
    int cy = (theme::SCREEN_H - kCardH) / 2;
    int btnY = cy + kCardH - 56;
    int gap  = 30;
    const int hitW = 140;
    int total = hitW * 3 + gap * 2;
    int bx = cx + (kCardW - total) / 2;
    if (tap && tap->active) {
        for (int i = 0; i < 3; ++i) {
            if (!pointInRect(tap, bx + i * (hitW + gap), btnY - 8, hitW, 36))
                continue;
            focus = i;
            return i == 0 ? ChoiceResult::Cancel
                          : (i == 1 ? ChoiceResult::Merge : ChoiceResult::Overwrite);
        }
    }
    if (kDown & HidNpadButton_B)
        return ChoiceResult::Cancel;
    if (kDown & HidNpadButton_A) {
        switch (focus) {
        case 0: return ChoiceResult::Cancel;
        case 1: return ChoiceResult::Merge;
        case 2: return ChoiceResult::Overwrite;
        }
    }
    if (kDown & HidNpadButton_AnyLeft) {
        focus--;
        if (focus < 0) focus = 2;
    }
    if (kDown & HidNpadButton_AnyRight) {
        focus++;
        if (focus > 2) focus = 0;
    }
    return ChoiceResult::None;
}

void ModalChoice::render(Renderer& renderer, FontManager& fm, const I18n& i18n) {
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

    const char* labels[3] = {
        i18n.t("modal.cancel"),
        i18n.t("modal.merge"),
        i18n.t("modal.overwrite"),
    };
    int btnY = cy + kCardH - 56;
    int gap  = 30;
    int ws[3];
    int total = 0;
    for (int i = 0; i < 3; i++) {
        ws[i] = fm.measureText(labels[i], FONT_SIZE_ITEM);
        total += ws[i];
    }
    total += gap * 2;
    int bx = cx + (kCardW - total) / 2;
    for (int i = 0; i < 3; i++) {
        SDL_Color col = (focus == i) ? PRIMARY : TEXT_SECONDARY;
        fm.drawText(renderer.sdl(), labels[i], bx, btnY, FONT_SIZE_ITEM, col);
        bx += ws[i] + gap;
    }
}

// ---- ModalProgress ----

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

void ModalProgress::render(Renderer& renderer, FontManager& fm, const I18n& i18n) {
    if (!active) return;
    using namespace theme;

    int cx = (SCREEN_W - kCardW) / 2;
    int cy = (SCREEN_H - 160) / 2;
    int h  = 160;

    renderer.drawRoundedRectFilled(cx, cy, kCardW, h, MENU_RADIUS, MENU_BG);
    renderer.drawRoundedRect(cx, cy, kCardW, h, MENU_RADIUS, MENU_BORDER);

    int tx = cx + kPad;
    int ty = cy + kPad;
    fm.drawText(renderer.sdl(), title.c_str(), tx, ty, FONT_SIZE_ITEM, TEXT);
    ty += FONT_SIZE_ITEM + 10;
    fm.drawTextEllipsis(renderer.sdl(), detail.c_str(), tx, ty, FONT_SIZE_SMALL, TEXT_SECONDARY,
                        kCardW - kPad * 2);
    ty += FONT_SIZE_SMALL + 14;
    const char* hint = i18n.t("modal.interrupt_hint");
    fm.drawText(renderer.sdl(), hint, tx, ty, FONT_SIZE_SMALL, TEXT_DISABLED);
}

// ---- ModalInfo ----

void ModalInfo::open(std::string t, std::string b, int bodyFontSizePx) {
    active = true;
    title  = std::move(t);
    body   = std::move(b);
    bodyFontSize = bodyFontSizePx > 0 ? bodyFontSizePx : theme::FONT_SIZE_SMALL;
}

void ModalInfo::close() {
    active = false;
    title.clear();
    body.clear();
    bodyFontSize = theme::FONT_SIZE_SMALL;
}

ConfirmResult ModalInfo::handleInput(uint64_t kDown, const TouchTap* tap) {
    if (!active) return ConfirmResult::None;
    int cardH = 420;
    int cx    = (theme::SCREEN_W - kCardW) / 2;
    int cy    = (theme::SCREEN_H - cardH) / 2;
    if (tap && tap->active &&
        ui::panelCloseButtonHit(cx, cy, kCardW, tap->x, tap->y))
        return ConfirmResult::Confirmed;
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

    ui::drawPanelTitleBar(renderer, fm, cx, cy, kCardW, title.c_str(), true, false);

    int tx = cx + kPad;
    int ty = cy + ui::kPanelTitleBarH + 12;

    const char* p = body.c_str();
    int lineH = fm.fontHeight(bodyFontSize) + 4;
    int maxY = cy + cardH - kPad;
    while (*p && ty < maxY) {
        const char* nl = strchr(p, '\n');
        std::string line(p, nl ? nl : p + strlen(p));
        fm.drawTextEllipsis(renderer.sdl(), line.c_str(), tx, ty, bodyFontSize,
                            TEXT_SECONDARY, kCardW - kPad * 2);
        ty += lineH;
        if (!nl) break;
        p = nl + 1;
    }
}

// ---- ModalInstallPrompt ----

void ModalInstallPrompt::open(std::string t, std::string b) {
    active = true;
    title  = std::move(t);
    body   = std::move(b);
    focus  = 0;
}

void ModalInstallPrompt::close() {
    active = false;
    title.clear();
    body.clear();
}

InstallPromptResult ModalInstallPrompt::handleInput(uint64_t kDown, const TouchTap* tap) {
    if (!active) return InstallPromptResult::None;
    int cx = (theme::SCREEN_W - kCardW) / 2;
    int cy = (theme::SCREEN_H - kCardH) / 2;
    int btnY = cy + kCardH - 56;
    const int hitW = 160;
    const int gap = 24;
    int total = hitW * 3 + gap * 2;
    int bx = cx + (kCardW - total) / 2;
    if (tap && tap->active) {
        for (int i = 0; i < 3; ++i) {
            if (!pointInRect(tap, bx + i * (hitW + gap), btnY - 8, hitW, 36))
                continue;
            focus = i;
            switch (i) {
            case 0: return InstallPromptResult::Cancel;
            case 1: return InstallPromptResult::Install;
            default: return InstallPromptResult::InstallAndDelete;
            }
        }
    }
    if (kDown & HidNpadButton_B)
        return InstallPromptResult::Cancel;
    if (kDown & HidNpadButton_A) {
        switch (focus) {
        case 0: return InstallPromptResult::Cancel;
        case 1: return InstallPromptResult::Install;
        case 2: return InstallPromptResult::InstallAndDelete;
        }
    }
    if (kDown & HidNpadButton_AnyLeft) {
        focus--;
        if (focus < 0) focus = 2;
    }
    if (kDown & HidNpadButton_AnyRight) {
        focus++;
        if (focus > 2) focus = 0;
    }
    return InstallPromptResult::None;
}

void ModalInstallPrompt::render(Renderer& renderer, FontManager& fm, const I18n& i18n) {
    if (!active) return;
    using namespace theme;

    int cardW = 760;
    int cardH = 240;
    int cx    = (SCREEN_W - cardW) / 2;
    int cy    = (SCREEN_H - cardH) / 2;

    renderer.drawRoundedRectFilled(cx, cy, cardW, cardH, MENU_RADIUS, MENU_BG);
    renderer.drawRoundedRect(cx, cy, cardW, cardH, MENU_RADIUS, MENU_BORDER);

    int tx = cx + kPad;
    int ty = cy + kPad;
    fm.drawText(renderer.sdl(), title.c_str(), tx, ty, FONT_SIZE_ITEM, TEXT);
    ty += FONT_SIZE_ITEM + 12;
    fm.drawTextEllipsis(renderer.sdl(), body.c_str(), tx, ty, FONT_SIZE_SMALL, TEXT_SECONDARY,
                        cardW - kPad * 2);

    const char* labels[3] = {
        i18n.t("installer.cancel"),
        i18n.t("installer.install"),
        i18n.t("installer.install_delete"),
    };
    int btnY = cy + cardH - 56;
    int gap  = 28;
    int ws[3];
    int total = 0;
    for (int i = 0; i < 3; i++) {
        ws[i] = fm.measureText(labels[i], FONT_SIZE_ITEM);
        total += ws[i];
    }
    total += gap * 2;
    int bx = cx + (cardW - total) / 2;
    for (int i = 0; i < 3; i++) {
        SDL_Color col = (focus == i) ? PRIMARY : TEXT_SECONDARY;
        if (i == 2 && focus != i)
            col = DANGER;
        fm.drawText(renderer.sdl(), labels[i], bx, btnY, FONT_SIZE_ITEM, col);
        bx += ws[i] + gap;
    }
}

} // namespace xplore
