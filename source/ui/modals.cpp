#include "fs/fs_api.hpp"
#include "ui/modals.hpp"
#include "ui/renderer.hpp"
#include "ui/font_manager.hpp"
#include "ui/panel_chrome.hpp"
#include "ui/theme.hpp"
#include "i18n/i18n.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <switch.h>

namespace xxplore {

static constexpr int kCardW = 560;
static constexpr int kCardH = 220;
static constexpr int kPad   = 20;
static constexpr int kInfoLinesPerPage = 8;
static constexpr int kOptionButtonH = 48;
static constexpr int kOptionTouchPadX = 16;
static constexpr int kOptionTouchPadY = 10;
static constexpr int kInfoActionButtonW = 132;
static constexpr int kInfoActionButtonGap = 10;

static void drawModalOptionButton(Renderer& renderer, FontManager& fm,
                                  int x, int y, int w, int h,
                                  const char* label, SDL_Color textColor,
                                  bool focused) {
    using namespace theme;
    SDL_Color bg = focused ? CURSOR_ROW : SURFACE_HOVER;
    SDL_Color border = focused ? PRIMARY_DIM : DIVIDER;
    renderer.drawRoundedRectFilled(x, y, w, h, 10, bg);
    renderer.drawRoundedRect(x, y, w, h, 10, border);
    int textW = fm.measureText(label, FONT_SIZE_ITEM);
    int textH = fm.fontHeight(FONT_SIZE_ITEM);
    int textX = x + (w - textW) / 2;
    int textY = y + (h - textH) / 2;
    fm.drawText(renderer.sdl(), label, textX, textY, FONT_SIZE_ITEM, textColor);
}

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
    int btnY = cy + kCardH - 68;
    int cancelW = 156;
    int okW = 156;
    int gap = 52;
    int total = cancelW + gap + okW;
    int bx = cx + (kCardW - total) / 2;
    if (tap && tap->active) {
        if (pointInRect(tap, bx - kOptionTouchPadX, btnY - kOptionTouchPadY,
                        cancelW + kOptionTouchPadX * 2, kOptionButtonH + kOptionTouchPadY * 2)) {
            focusOk = 0;
            return ConfirmResult::Cancelled;
        }
        if (pointInRect(tap, bx + cancelW + gap - kOptionTouchPadX, btnY - kOptionTouchPadY,
                        okW + kOptionTouchPadX * 2, kOptionButtonH + kOptionTouchPadY * 2)) {
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

void ModalConfirm::render(Renderer& renderer, FontManager& fm, const I18n& i18n) const {
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
    int btnY             = cy + kCardH - 68;
    int okW              = 156;
    int cancelW          = 156;
    int gap              = 52;
    int total            = cancelW + gap + okW;
    int bx               = cx + (kCardW - total) / 2;

    SDL_Color canCol   = (focusOk == 0) ? PRIMARY : TEXT_SECONDARY;
    SDL_Color okCol    = (focusOk == 1) ? PRIMARY : TEXT_SECONDARY;
    drawModalOptionButton(renderer, fm, bx, btnY, cancelW, kOptionButtonH, cancelTxt, canCol,
                          focusOk == 0);
    drawModalOptionButton(renderer, fm, bx + cancelW + gap, btnY, okW, kOptionButtonH, okTxt,
                          okCol, focusOk == 1);
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
    int btnY = cy + kCardH - 68;
    int gap  = 24;
    const int hitW = 164;
    int total = hitW * 3 + gap * 2;
    int bx = cx + (kCardW - total) / 2;
    if (tap && tap->active) {
        for (int i = 0; i < 3; ++i) {
            if (!pointInRect(tap, bx + i * (hitW + gap) - kOptionTouchPadX,
                             btnY - kOptionTouchPadY,
                             hitW + kOptionTouchPadX * 2,
                             kOptionButtonH + kOptionTouchPadY * 2))
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
    int btnY = cy + kCardH - 68;
    int gap  = 24;
    int total = 164 * 3 + gap * 2;
    int bx = cx + (kCardW - total) / 2;
    for (int i = 0; i < 3; i++) {
        SDL_Color col = (focus == i) ? PRIMARY : TEXT_SECONDARY;
        drawModalOptionButton(renderer, fm, bx, btnY, 164, kOptionButtonH, labels[i], col,
                              focus == i);
        bx += 164 + gap;
    }
}

// ---- ModalProgress ----

void ModalProgress::open(std::string t, std::string d) {
    active = true;
    title  = std::move(t);
    detail = std::move(d);
    progressCurrent = 0;
    progressTotal = 0;
}

void ModalProgress::close() {
    active = false;
    title.clear();
    detail.clear();
    progressCurrent = 0;
    progressTotal = 0;
}

void ModalProgress::render(Renderer& renderer, FontManager& fm, const I18n& i18n) {
    if (!active) return;
    using namespace theme;

    int cx = (SCREEN_W - kCardW) / 2;
    int cy = (SCREEN_H - 210) / 2;
    int h  = 210;

    renderer.drawRoundedRectFilled(cx, cy, kCardW, h, MENU_RADIUS, MENU_BG);
    renderer.drawRoundedRect(cx, cy, kCardW, h, MENU_RADIUS, MENU_BORDER);

    int tx = cx + kPad;
    int ty = cy + kPad;
    fm.drawText(renderer.sdl(), title.c_str(), tx, ty, FONT_SIZE_ITEM, TEXT);
    ty += FONT_SIZE_ITEM + 10;
    fm.drawTextEllipsis(renderer.sdl(), detail.c_str(), tx, ty, FONT_SIZE_SMALL, TEXT_SECONDARY,
                        kCardW - kPad * 2);
    ty += FONT_SIZE_SMALL + 16;

    if (progressTotal > 0) {
        constexpr int kBarH = 18;
        int barW = kCardW - kPad * 2;
        renderer.drawRoundedRectFilled(tx, ty, barW, kBarH, 8, SURFACE);
        renderer.drawRoundedRect(tx, ty, barW, kBarH, 8, DIVIDER);
        int fillW = static_cast<int>((static_cast<double>(progressCurrent) /
                                      static_cast<double>(progressTotal)) * barW);
        if (fillW < 0)
            fillW = 0;
        if (fillW > barW)
            fillW = barW;
        if (fillW > 0)
            renderer.drawRoundedRectFilled(tx, ty, fillW, kBarH, 8, PRIMARY);

        char percentBuf[32];
        int percent = static_cast<int>((progressCurrent * 100ULL) / progressTotal);
        std::snprintf(percentBuf, sizeof(percentBuf), "%d%%", percent);
        std::string bytes = fs::formatSize(progressCurrent);
        bytes += " / ";
        bytes += fs::formatSize(progressTotal);
        int percentY = ty + kBarH + 10;
        fm.drawText(renderer.sdl(), bytes.c_str(), tx, percentY, FONT_SIZE_SMALL,
                    TEXT_SECONDARY);
        int percentW = fm.measureText(percentBuf, FONT_SIZE_SMALL);
        fm.drawText(renderer.sdl(), percentBuf, tx + barW - percentW, percentY,
                    FONT_SIZE_SMALL, TEXT_SECONDARY);
        ty = percentY + FONT_SIZE_SMALL + 14;
    }

    const char* hint = i18n.t("modal.interrupt_hint");
    fm.drawText(renderer.sdl(), hint, tx, ty, FONT_SIZE_SMALL, TEXT_DISABLED);
}

// ---- ModalErrorAction ----

void ModalErrorAction::open(std::string t, std::string b, std::string d) {
    active = true;
    title = std::move(t);
    body = std::move(b);
    detail = std::move(d);
    focus = 0;
}

void ModalErrorAction::close() {
    active = false;
    title.clear();
    body.clear();
    detail.clear();
}

ErrorActionResult ModalErrorAction::handleInput(uint64_t kDown, const TouchTap* tap) {
    if (!active) return ErrorActionResult::None;
    int cardW = 760;
    int cardH = 250;
    int cx = (theme::SCREEN_W - cardW) / 2;
    int cy = (theme::SCREEN_H - cardH) / 2;
    int btnY = cy + cardH - 68;
    const int hitW = 176;
    const int gap = 28;
    int total = hitW * 3 + gap * 2;
    int bx = cx + (cardW - total) / 2;
    if (tap && tap->active) {
        for (int i = 0; i < 3; ++i) {
            if (!pointInRect(tap, bx + i * (hitW + gap) - kOptionTouchPadX,
                             btnY - kOptionTouchPadY,
                             hitW + kOptionTouchPadX * 2,
                             kOptionButtonH + kOptionTouchPadY * 2))
                continue;
            focus = i;
            switch (i) {
            case 0: return ErrorActionResult::Abort;
            case 1: return ErrorActionResult::Ignore;
            default: return ErrorActionResult::IgnoreAll;
            }
        }
    }
    if (kDown & HidNpadButton_B)
        return ErrorActionResult::Abort;
    if (kDown & HidNpadButton_A) {
        switch (focus) {
        case 0: return ErrorActionResult::Abort;
        case 1: return ErrorActionResult::Ignore;
        case 2: return ErrorActionResult::IgnoreAll;
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
    return ErrorActionResult::None;
}

void ModalErrorAction::render(Renderer& renderer, FontManager& fm, const I18n& i18n) {
    if (!active) return;
    using namespace theme;

    int cardW = 760;
    int cardH = 250;
    int cx = (SCREEN_W - cardW) / 2;
    int cy = (SCREEN_H - cardH) / 2;

    renderer.drawRoundedRectFilled(cx, cy, cardW, cardH, MENU_RADIUS, MENU_BG);
    renderer.drawRoundedRect(cx, cy, cardW, cardH, MENU_RADIUS, MENU_BORDER);

    int tx = cx + kPad;
    int ty = cy + kPad;
    fm.drawText(renderer.sdl(), title.c_str(), tx, ty, FONT_SIZE_ITEM, TEXT);
    ty += FONT_SIZE_ITEM + 12;
    fm.drawTextEllipsis(renderer.sdl(), body.c_str(), tx, ty, FONT_SIZE_SMALL, TEXT_SECONDARY,
                        cardW - kPad * 2);
    ty += FONT_SIZE_SMALL + 10;
    fm.drawTextEllipsis(renderer.sdl(), detail.c_str(), tx, ty, FONT_SIZE_SMALL, DANGER,
                        cardW - kPad * 2);

    const char* labels[3] = {
        i18n.t("modal.abort"),
        i18n.t("modal.ignore"),
        i18n.t("modal.ignore_all"),
    };
    int btnY = cy + cardH - 68;
    int gap = 28;
    int total = 176 * 3 + gap * 2;
    int bx = cx + (cardW - total) / 2;
    for (int i = 0; i < 3; ++i) {
        SDL_Color col = (focus == i) ? PRIMARY : TEXT_SECONDARY;
        if (i == 0 && focus != i)
            col = DANGER;
        drawModalOptionButton(renderer, fm, bx, btnY, 176, kOptionButtonH, labels[i], col,
                              focus == i);
        bx += 176 + gap;
    }
}

// ---- ModalInfo ----

void ModalInfo::open(std::string t, std::string b, int bodyFontSizePx,
                     std::string actionLabel) {
    active = true;
    title  = std::move(t);
    body   = std::move(b);
    actionButtonLabel = std::move(actionLabel);
    bodyFontSize = bodyFontSizePx > 0 ? bodyFontSizePx : theme::FONT_SIZE_SMALL;
    lines.clear();
    pageIndex = 0;
    actionRequested = false;

    const char* p = body.c_str();
    while (true) {
        const char* nl = std::strchr(p, '\n');
        lines.emplace_back(p, nl ? nl : p + std::strlen(p));
        if (!nl)
            break;
        p = nl + 1;
    }
}

void ModalInfo::close() {
    active = false;
    title.clear();
    body.clear();
    actionButtonLabel.clear();
    bodyFontSize = theme::FONT_SIZE_SMALL;
    lines.clear();
    pageIndex = 0;
    actionRequested = false;
}

ConfirmResult ModalInfo::handleInput(uint64_t kDown, const TouchTap* tap) {
    if (!active) return ConfirmResult::None;
    int cardH = 420;
    int cx    = (theme::SCREEN_W - kCardW) / 2;
    int cy    = (theme::SCREEN_H - cardH) / 2;
    int pagerH = 36;
    int pageCount = static_cast<int>((lines.size() + kInfoLinesPerPage - 1) / kInfoLinesPerPage);
    if (pageCount < 1)
        pageCount = 1;
    if (pageIndex >= pageCount)
        pageIndex = pageCount - 1;

    int pagerY = cy + cardH - kPad - pagerH;
    int navW = 56;
    const bool hasActionButton = !actionButtonLabel.empty();
    const int actionButtonHostW = kCardW - ui::kPanelCloseButtonSize - kInfoActionButtonGap;
    if (tap && tap->active &&
        ui::panelCloseButtonHit(cx, cy, kCardW, tap->x, tap->y))
        return ConfirmResult::Confirmed;
    if (tap && tap->active && hasActionButton &&
        ui::panelTextButtonHit(cx, cy, actionButtonHostW, kInfoActionButtonW, tap->x, tap->y)) {
        actionRequested = true;
        return ConfirmResult::Confirmed;
    }
    if (tap && tap->active && pageCount > 1) {
        if (pointInRect(tap, cx + kPad, pagerY, navW, pagerH) && pageIndex > 0) {
            pageIndex--;
            return ConfirmResult::None;
        }
        if (pointInRect(tap, cx + kCardW - kPad - navW, pagerY, navW, pagerH) &&
            pageIndex + 1 < pageCount) {
            pageIndex++;
            return ConfirmResult::None;
        }
    }
    if ((kDown & (HidNpadButton_AnyLeft | HidNpadButton_L)) && pageIndex > 0) {
        pageIndex--;
        return ConfirmResult::None;
    }
    if ((kDown & (HidNpadButton_AnyRight | HidNpadButton_R)) && pageIndex + 1 < pageCount) {
        pageIndex++;
        return ConfirmResult::None;
    }
    if ((kDown & HidNpadButton_Minus) && hasActionButton) {
        actionRequested = true;
        return ConfirmResult::Confirmed;
    }
    if (kDown & (HidNpadButton_A | HidNpadButton_B))
        return ConfirmResult::Confirmed;
    return ConfirmResult::None;
}

bool ModalInfo::takeActionRequested() {
    bool value = actionRequested;
    actionRequested = false;
    return value;
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
    if (!actionButtonLabel.empty()) {
        ui::drawPanelTextButton(renderer, fm, cx, cy,
                                kCardW - ui::kPanelCloseButtonSize - kInfoActionButtonGap,
                                kInfoActionButtonW, actionButtonLabel.c_str(), false);
    }

    int tx = cx + kPad;
    int ty = cy + ui::kPanelTitleBarH + 12;
    int lineH = fm.fontHeight(bodyFontSize) + 4;
    int pagerH = 36;
    int pageCount = static_cast<int>((lines.size() + kInfoLinesPerPage - 1) / kInfoLinesPerPage);
    if (pageCount < 1)
        pageCount = 1;
    int startLine = pageIndex * kInfoLinesPerPage;
    int endLine = startLine + kInfoLinesPerPage;
    if (endLine > static_cast<int>(lines.size()))
        endLine = static_cast<int>(lines.size());
    for (int i = startLine; i < endLine; ++i) {
        fm.drawTextEllipsis(renderer.sdl(), lines[i].c_str(), tx, ty, bodyFontSize,
                            TEXT_SECONDARY, kCardW - kPad * 2);
        ty += lineH;
    }

    if (pageCount > 1) {
        int pagerY = cy + cardH - kPad - pagerH;
        int navW = 56;
        renderer.drawRoundedRectFilled(cx + kPad, pagerY, navW, pagerH, 8, SURFACE);
        renderer.drawRoundedRect(cx + kPad, pagerY, navW, pagerH, 8,
                                 pageIndex > 0 ? PRIMARY_DIM : DIVIDER);
        renderer.drawRoundedRectFilled(cx + kCardW - kPad - navW, pagerY, navW, pagerH, 8,
                                       SURFACE);
        renderer.drawRoundedRect(cx + kCardW - kPad - navW, pagerY, navW, pagerH, 8,
                                 pageIndex + 1 < pageCount ? PRIMARY_DIM : DIVIDER);
        SDL_Color prevCol = pageIndex > 0 ? PRIMARY : TEXT_DISABLED;
        SDL_Color nextCol = (pageIndex + 1 < pageCount) ? PRIMARY : TEXT_DISABLED;
        fm.drawText(renderer.sdl(), "<", cx + kPad + 20, pagerY + 8, FONT_SIZE_ITEM, prevCol);
        fm.drawText(renderer.sdl(), ">", cx + kCardW - kPad - navW + 20, pagerY + 8,
                    FONT_SIZE_ITEM, nextCol);

        char pageBuf[32];
        std::snprintf(pageBuf, sizeof(pageBuf), "%d/%d", pageIndex + 1, pageCount);
        int pageW = fm.measureText(pageBuf, FONT_SIZE_SMALL);
        int pageX = cx + (kCardW - pageW) / 2;
        int pageTextH = fm.fontHeight(FONT_SIZE_SMALL);
        int pageTextY = pagerY + (pagerH - pageTextH) / 2;
        fm.drawText(renderer.sdl(), pageBuf, pageX, pageTextY, FONT_SIZE_SMALL, TEXT_DISABLED);
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
    int btnY = cy + kCardH - 68;
    const int hitW = 176;
    const int gap = 28;
    int total = hitW * 3 + gap * 2;
    int bx = cx + (kCardW - total) / 2;
    if (tap && tap->active) {
        for (int i = 0; i < 3; ++i) {
            if (!pointInRect(tap, bx + i * (hitW + gap) - kOptionTouchPadX,
                             btnY - kOptionTouchPadY,
                             hitW + kOptionTouchPadX * 2,
                             kOptionButtonH + kOptionTouchPadY * 2))
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
    int btnY = cy + cardH - 68;
    int gap  = 28;
    int total = 176 * 3 + gap * 2;
    int bx = cx + (cardW - total) / 2;
    for (int i = 0; i < 3; i++) {
        SDL_Color col = (focus == i) ? PRIMARY : TEXT_SECONDARY;
        if (i == 2 && focus != i)
            col = DANGER;
        drawModalOptionButton(renderer, fm, bx, btnY, 176, kOptionButtonH, labels[i], col,
                              focus == i);
        bx += 176 + gap;
    }
}

// ---- ModalOptionList ----

void ModalOptionList::open(std::string t, std::string b, std::vector<ModalOptionListEntry> opts,
                           int cancelOption, int initialFocus) {
    active = true;
    title = std::move(t);
    body = std::move(b);
    options = std::move(opts);
    cancelIndex = cancelOption;
    focus = options.empty() ? 0
                            : std::clamp(initialFocus, 0,
                                         static_cast<int>(options.size()) - 1);
    if (!options.empty() && !options[focus].enabled)
        moveFocus(1);
}

void ModalOptionList::close() {
    active = false;
    title.clear();
    body.clear();
    options.clear();
    focus = 0;
    cancelIndex = -1;
}

void ModalOptionList::moveFocus(int delta) {
    if (options.empty() || delta == 0)
        return;
    int next = focus;
    for (int i = 0; i < static_cast<int>(options.size()); ++i) {
        next += delta;
        if (next < 0)
            next = static_cast<int>(options.size()) - 1;
        if (next >= static_cast<int>(options.size()))
            next = 0;
        if (options[next].enabled) {
            focus = next;
            return;
        }
    }
}

int ModalOptionList::handleInput(uint64_t kDown, const TouchTap* tap) {
    if (!active)
        return -1;

    const int cardW = 760;
    const int optionW = cardW - kPad * 2;
    int bodyLineCount = 0;
    if (!body.empty())
        bodyLineCount = body.find('\n') == std::string::npos ? 1 : 2;
    const int optionGap = 12;
    const int cardH = 120 + bodyLineCount * (theme::FONT_SIZE_SMALL + 8) +
                      static_cast<int>(options.size()) * (kOptionButtonH + optionGap) + 8;
    const int cx = (theme::SCREEN_W - cardW) / 2;
    const int cy = (theme::SCREEN_H - cardH) / 2;
    const int optionX = cx + kPad;
    int optionY = cy + kPad + theme::FONT_SIZE_ITEM + 18;
    if (bodyLineCount > 0)
        optionY += bodyLineCount * (theme::FONT_SIZE_SMALL + 8) + 8;

    if (tap && tap->active) {
        for (int i = 0; i < static_cast<int>(options.size()); ++i) {
            if (!pointInRect(tap, optionX - kOptionTouchPadX, optionY - kOptionTouchPadY,
                             optionW + kOptionTouchPadX * 2,
                             kOptionButtonH + kOptionTouchPadY * 2)) {
                optionY += kOptionButtonH + optionGap;
                continue;
            }
            focus = i;
            if (options[i].enabled)
                return i;
            return -1;
        }
    }

    if ((kDown & HidNpadButton_B) && cancelIndex >= 0 &&
        cancelIndex < static_cast<int>(options.size()) && options[cancelIndex].enabled) {
        focus = cancelIndex;
        return cancelIndex;
    }
    if (kDown & HidNpadButton_A) {
        if (focus >= 0 && focus < static_cast<int>(options.size()) && options[focus].enabled)
            return focus;
        return -1;
    }
    if (kDown & HidNpadButton_AnyUp)
        moveFocus(-1);
    if (kDown & HidNpadButton_AnyDown)
        moveFocus(1);
    return -1;
}

void ModalOptionList::render(Renderer& renderer, FontManager& fm) const {
    if (!active)
        return;

    using namespace theme;
    const int cardW = 760;
    const int optionW = cardW - kPad * 2;
    int bodyLineCount = 0;
    if (!body.empty())
        bodyLineCount = body.find('\n') == std::string::npos ? 1 : 2;
    const int optionGap = 12;
    const int cardH = 120 + bodyLineCount * (FONT_SIZE_SMALL + 8) +
                      static_cast<int>(options.size()) * (kOptionButtonH + optionGap) + 8;
    const int cx = (SCREEN_W - cardW) / 2;
    const int cy = (SCREEN_H - cardH) / 2;

    renderer.drawRoundedRectFilled(cx, cy, cardW, cardH, MENU_RADIUS, MENU_BG);
    renderer.drawRoundedRect(cx, cy, cardW, cardH, MENU_RADIUS, MENU_BORDER);

    int tx = cx + kPad;
    int ty = cy + kPad;
    fm.drawText(renderer.sdl(), title.c_str(), tx, ty, FONT_SIZE_ITEM, TEXT);
    ty += FONT_SIZE_ITEM + 12;

    if (!body.empty()) {
        std::size_t split = body.find('\n');
        std::string line1 = split == std::string::npos ? body : body.substr(0, split);
        std::string line2 = split == std::string::npos ? "" : body.substr(split + 1);
        fm.drawTextEllipsis(renderer.sdl(), line1.c_str(), tx, ty, FONT_SIZE_SMALL,
                            TEXT_SECONDARY, cardW - kPad * 2);
        ty += FONT_SIZE_SMALL + 8;
        if (!line2.empty()) {
            fm.drawTextEllipsis(renderer.sdl(), line2.c_str(), tx, ty, FONT_SIZE_SMALL,
                                TEXT_SECONDARY, cardW - kPad * 2);
            ty += FONT_SIZE_SMALL + 8;
        }
        ty += 8;
    }

    for (int i = 0; i < static_cast<int>(options.size()); ++i) {
        SDL_Color textColor = options[i].enabled
            ? (focus == i ? PRIMARY : TEXT)
            : TEXT_DISABLED;
        drawModalOptionButton(renderer, fm, tx, ty, optionW, kOptionButtonH,
                              options[i].label.c_str(), textColor,
                              focus == i && options[i].enabled);
        ty += kOptionButtonH + optionGap;
    }
}

} // namespace xxplore
