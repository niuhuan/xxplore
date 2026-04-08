#include "ui/main_menu.hpp"
#include "ui/renderer.hpp"
#include "ui/font_manager.hpp"
#include "ui/panel_chrome.hpp"
#include "ui/theme.hpp"
#include "i18n/i18n.hpp"
#include <switch.h>
#include <cstdio>
#include <cstring>

namespace xplore {

static float easeOutCubic(float t) {
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

void BottomMainMenu::open() {
    open_     = true;
    anim_     = 0.0f;
    focusRow_ = 1;
    focusCol_ = 0;
    pending_  = MenuCommand::None;
}

void BottomMainMenu::close() {
    open_ = false;
    anim_ = 0.0f;
}

MenuCommand BottomMainMenu::takeCommand() {
    MenuCommand c = pending_;
    pending_      = MenuCommand::None;
    return c;
}

MenuCommand BottomMainMenu::cmdAt(int row, int col, const MainMenuState& st) const {
    if (row == 2) {
        MenuCommand r1 = st.renameIsEdit   ? MenuCommand::EditDrive   : MenuCommand::Rename;
        MenuCommand d1 = st.deleteIsUnmount ? MenuCommand::UnmountDrive :
                         (st.deleteIsDriveDel ? MenuCommand::DeleteDrive : MenuCommand::Delete);
        const MenuCommand m[] = {MenuCommand::ToggleSelectMode, r1,
                                 MenuCommand::NewFolder, d1};
        return m[col];
    }
    if (row == 3) {
        const MenuCommand m[] = {MenuCommand::Copy, MenuCommand::Cut, MenuCommand::Paste,
                                 MenuCommand::None};
        return m[col];
    }
    if (row == 4) {
        const MenuCommand m[] = {MenuCommand::ViewClipboard, MenuCommand::ClearClipboard,
                                 MenuCommand::InstallApplications, MenuCommand::None};
        return m[col];
    }
    if (row == 5) {
        const MenuCommand m[] = {MenuCommand::Settings, MenuCommand::Help, MenuCommand::About,
                                 MenuCommand::ExitApp};
        return m[col];
    }
    return MenuCommand::None;
}

bool BottomMainMenu::cellDisabled(int row, int col, const MainMenuState& st) const {
    if (row == 1) return false;
    if (row == 2) {
        const bool d[] = {st.disableSelectToggle, st.disableRename, st.disableNewFolder,
                          st.disableDelete};
        return d[col];
    }
    if (row == 3) {
        const bool d[] = {st.disableCopy, st.disableCut, st.disablePaste, true};
        return d[col];
    }
    if (row == 4) {
        const bool d[] = {st.disableViewClip, st.disableClearClip, st.disableInstall, true};
        return d[col];
    }
    if (row == 5) {
        const bool d[] = {st.disableSettings, st.disableHelp, st.disableAbout, st.disableExit};
        return d[col];
    }
    return true;
}

bool BottomMainMenu::tryFocusAt(int r, int c, const MainMenuState& st) {
    if (r == 1) {
        focusRow_ = 1;
        focusCol_ = 0;
        return true;
    }
    if (r < 2 || r > 5 || c < 0 || c > 3) return false;
    if (cellDisabled(r, c, st)) return false;
    if (cmdAt(r, c, st) == MenuCommand::None) return false;
    focusRow_ = r;
    focusCol_ = c;
    return true;
}

void BottomMainMenu::moveFocusVertical(int delta, const MainMenuState& st) {
    if (delta == 0) return;
    int r = focusRow_;
    int c = focusCol_;
    for (int i = 0; i < 8; i++) {
        r += delta;
        if (r < 1) r = 5;
        if (r > 5) r = 1;
        if (r == 1) {
            focusRow_ = 1;
            focusCol_ = 0;
            return;
        }
        if (tryFocusAt(r, c, st)) return;
        if (tryFocusAt(r, 0, st)) return;
        if (tryFocusAt(r, 1, st)) return;
        if (tryFocusAt(r, 2, st)) return;
        if (tryFocusAt(r, 3, st)) return;
    }
}

void BottomMainMenu::moveFocusHorizontal(int delta, const MainMenuState& st) {
    if (delta == 0 || focusRow_ == 1) return;
    int c = focusCol_;
    for (int i = 0; i < 8; i++) {
        c += delta;
        if (c < 0) c = 3;
        if (c > 3) c = 0;
        if (tryFocusAt(focusRow_, c, st)) return;
    }
}

void BottomMainMenu::activateCell(const MainMenuState& st) {
    if (focusRow_ == 1) {
        pending_ = MenuCommand::CloseMenu;
        return;
    }
    if (cellDisabled(focusRow_, focusCol_, st)) return;
    MenuCommand cmd = cmdAt(focusRow_, focusCol_, st);
    if (cmd != MenuCommand::None)
        pending_ = cmd;
}

void BottomMainMenu::update(uint32_t deltaMs, uint64_t kDown, const MainMenuState& st,
                            const TouchTap* tap) {
    if (!open_) return;

    float dur = static_cast<float>(theme::MENU_SHEET_ANIM_MS);
    anim_ += static_cast<float>(deltaMs) / dur;
    if (anim_ > 1.0f) anim_ = 1.0f;

    if (kDown & HidNpadButton_B) {
        pending_ = MenuCommand::CloseMenu;
        return;
    }

    if (anim_ < 1.0f) return;

    if (tap && tap->active) {
        int mx = theme::MENU_SHEET_MARGIN_X;
        int mw = theme::SCREEN_W - mx * 2;
        int sh = theme::MENU_SHEET_TITLE_H + theme::MENU_SHEET_CONTEXT_H + 1 +
                 theme::MENU_SHEET_ROWS * theme::MENU_SHEET_CELL_H + theme::MENU_PADDING * 2;
        int y = theme::SCREEN_H - theme::FOOTER_H - sh;
        if (ui::panelCloseButtonHit(mx, y, mw, tap->x, tap->y)) {
            pending_ = MenuCommand::CloseMenu;
            return;
        }

        int ty = y + theme::MENU_PADDING;
        int ctxY = ty + theme::MENU_SHEET_TITLE_H;
        int ctxInnerW = mw - theme::MENU_PADDING * 2;
        if (pointInRect(tap, mx + theme::MENU_PADDING, ctxY, ctxInnerW, theme::MENU_SHEET_CONTEXT_H)) {
            focusRow_ = 1;
            focusCol_ = 0;
            pending_ = MenuCommand::CloseMenu;
            return;
        }

        int cellW = (mw - theme::MENU_PADDING * 2) / 4;
        int gridTop = ty + theme::MENU_SHEET_TITLE_H + theme::MENU_SHEET_CONTEXT_H + 1;
        for (int row = 2; row <= 5; ++row) {
            for (int col = 0; col < 4; ++col) {
                int cx = mx + theme::MENU_PADDING + col * cellW;
                int cy = gridTop + (row - 2) * theme::MENU_SHEET_CELL_H;
                if (!pointInRect(tap, cx, cy, cellW, theme::MENU_SHEET_CELL_H))
                    continue;
                if (cellDisabled(row, col, st))
                    return;
                MenuCommand cmd = cmdAt(row, col, st);
                if (cmd == MenuCommand::None)
                    return;
                focusRow_ = row;
                focusCol_ = col;
                pending_ = cmd;
                return;
            }
        }
    }

    if (kDown & HidNpadButton_AnyUp) moveFocusVertical(-1, st);
    if (kDown & HidNpadButton_AnyDown) moveFocusVertical(1, st);
    if (kDown & HidNpadButton_AnyLeft) moveFocusHorizontal(-1, st);
    if (kDown & HidNpadButton_AnyRight) moveFocusHorizontal(1, st);
    if (kDown & HidNpadButton_A) activateCell(st);
}

static const char* labelKey(int row, int col, const MainMenuState& st) {
    if (row == 2) {
        const char* rk = st.renameIsEdit    ? "menu.edit"         : "menu.rename";
        const char* dk = st.deleteIsUnmount ? "menu.unmount_drive" :
                         (st.deleteIsDriveDel ? "menu.delete_drive" : "menu.delete");
        const char* k[] = {"menu.select_toggle", rk, "menu.new_folder", dk};
        return k[col];
    }
    if (row == 3) {
        const char* pasteKey = st.pasteFromCut ? "menu.paste_move_here" : "menu.paste_copy_here";
        const char* k[] = {"menu.copy", "menu.cut", pasteKey, ""};
        return k[col];
    }
    if (row == 4) {
        const char* k[] = {"menu.clipboard_view", "menu.clipboard_clear",
                           "menu.install_applications", ""};
        return k[col];
    }
    if (row == 5) {
        const char* k[] = {"menu.settings", "menu.help", "menu.about", "menu.exit"};
        return k[col];
    }
    return "";
}

void BottomMainMenu::render(Renderer& renderer, FontManager& fm, const I18n& i18n,
                            const MainMenuState& st) {
    if (!open_ && anim_ <= 0.0f) return;

    using namespace theme;
    float t    = easeOutCubic(anim_);
    int mx     = MENU_SHEET_MARGIN_X;
    int mw     = SCREEN_W - mx * 2;
    int sh     = MENU_SHEET_TITLE_H + MENU_SHEET_CONTEXT_H + 1 +
             MENU_SHEET_ROWS * MENU_SHEET_CELL_H + MENU_PADDING * 2;
    // Keep bottom tips bar visible; sheet ends above FOOTER_H.
    int y0     = SCREEN_H - FOOTER_H - sh;
    int slide  = static_cast<int>(static_cast<float>(sh + 40) * (1.0f - t));
    int y      = y0 + slide;

    // --- Sheet background with subtle shadow ---
    // Shadow layer (offset down, slightly larger, semi-transparent)
    constexpr SDL_Color SHADOW = {0x00, 0x00, 0x00, 0x40};
    renderer.drawRoundedRectFilled(mx + 2, y + 3, mw, sh, MENU_RADIUS, SHADOW);
    // Main card
    renderer.drawRoundedRectFilled(mx, y, mw, sh, MENU_RADIUS, MENU_BG);
    renderer.drawRoundedRect(mx, y, mw, sh, MENU_RADIUS, MENU_BORDER);

    int tx = mx + MENU_PADDING;
    int ty = y + MENU_PADDING;

    // --- Title row ---
    ui::drawPanelTitleBar(renderer, fm, mx, y, mw, i18n.t("menu.title"), true, false);
    fm.drawText(renderer.sdl(), i18n.t("menu.hints"), tx + PADDING_SM,
                ty + MENU_SHEET_TITLE_H - FONT_SIZE_SMALL - 8, FONT_SIZE_SMALL, TEXT_SECONDARY);
    ty += MENU_SHEET_TITLE_H;

    // --- Context row (current folder) ---
    int ctxY = ty;
    int ctxInnerW = mw - MENU_PADDING * 2;
    // Context row background — subtle surface tint so it stands out
    renderer.drawRoundedRectFilled(mx + MENU_PADDING, ctxY, ctxInnerW, MENU_SHEET_CONTEXT_H, 8,
                                   SURFACE);
    if (focusRow_ == 1) {
        renderer.drawRoundedRectFilled(mx + MENU_PADDING, ctxY, ctxInnerW, MENU_SHEET_CONTEXT_H, 8,
                                       CURSOR_ROW);
        renderer.drawRoundedRect(mx + MENU_PADDING, ctxY, ctxInnerW, MENU_SHEET_CONTEXT_H, 8,
                                 PRIMARY_DIM);
    }
    int textX = tx + PADDING_SM;
    SDL_Color ctxCol = (focusRow_ == 1) ? PRIMARY : TEXT;
    fm.drawTextEllipsis(renderer.sdl(), st.contextLine.c_str(), textX,
                        ctxY + (MENU_SHEET_CONTEXT_H - FONT_SIZE_ITEM) / 2, FONT_SIZE_ITEM, ctxCol,
                        mw - MENU_PADDING * 2 - PADDING_SM * 2);
    ty += MENU_SHEET_CONTEXT_H;

    // --- Divider below context row ---
    int divX1 = mx + MENU_PADDING + 4;
    int divX2 = mx + mw - MENU_PADDING - 4;
    renderer.drawLine(divX1, ty, divX2, ty, DIVIDER);
    ty += 1;

    // --- Grid cells ---
    int cellW = (mw - MENU_PADDING * 2) / 4;
    for (int row = 2; row <= 5; row++) {
        // Divider between row groups (before rows 3, 4, 5)
        if (row > 2) {
            int divY = ty + (row - 2) * MENU_SHEET_CELL_H;
            renderer.drawLine(divX1, divY, divX2, divY, DIVIDER);
        }
        for (int col = 0; col < 4; col++) {
            int cx = mx + MENU_PADDING + col * cellW;
            int cy = ty + (row - 2) * MENU_SHEET_CELL_H;
            bool dis = cellDisabled(row, col, st);
            bool foc = (focusRow_ == row && focusCol_ == col);
            if (foc) {
                renderer.drawRoundedRectFilled(cx + 2, cy + 2, cellW - 4, MENU_SHEET_CELL_H - 4, 8,
                                               CURSOR_ROW);
                renderer.drawRoundedRect(cx + 2, cy + 2, cellW - 4, MENU_SHEET_CELL_H - 4, 8,
                                         PRIMARY_DIM);
            }
            const char* key = labelKey(row, col, st);
            if (!key || !key[0]) continue;
            SDL_Color colr = dis ? TEXT_DISABLED : (foc ? PRIMARY : MENU_ITEM_TEXT);
            fm.drawTextEllipsis(renderer.sdl(), i18n.t(key), cx + 8,
                                cy + (MENU_SHEET_CELL_H - FONT_SIZE_SMALL) / 2, FONT_SIZE_SMALL,
                                colr, cellW - 16);
        }
    }
}

} // namespace xplore
