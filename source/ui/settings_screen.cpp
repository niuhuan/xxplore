#include "ui/settings_screen.hpp"
#include "i18n/i18n.hpp"
#include "ui/font_manager.hpp"
#include "ui/panel_chrome.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "util/app_config.hpp"
#include <SDL.h>
#include <string>
#include <switch.h>

namespace xplore {

namespace {

struct LanguageCell {
    config::AppLanguage language;
    const char*         label;
};

constexpr LanguageCell kLanguages[] = {
    {config::AppLanguage::ZhCn, u8"简体中文"},
    {config::AppLanguage::ZhTw, u8"繁體中文"},
    {config::AppLanguage::En,   "English"},
    {config::AppLanguage::Ja,   u8"日本語"},
    {config::AppLanguage::Ko,   u8"한국어"},
    {config::AppLanguage::Fr,   u8"Français"},
    {config::AppLanguage::Ru,   u8"Русский"},
    {config::AppLanguage::Es,   u8"Español"},
};

SDL_Color withAlpha(SDL_Color color, Uint8 alpha) {
    color.a = alpha;
    return color;
}

int languageIndexFromValue(config::AppLanguage language) {
    for (int i = 0; i < static_cast<int>(sizeof(kLanguages) / sizeof(kLanguages[0])); ++i) {
        if (kLanguages[i].language == language)
            return i;
    }
    return 0;
}

} // namespace

void SettingsScreen::open(config::AppLanguage currentLanguage) {
    open_ = true;
    currentLanguage_ = currentLanguage;
    selectedLanguage_ = currentLanguage;
    int index = languageIndexFromValue(currentLanguage);
    focusRow_ = 0;
    languageFocusCol_ = index;
    buttonFocusCol_ = 0;
}

void SettingsScreen::close() {
    open_ = false;
}

int SettingsScreen::languageIndex(int col) const {
    if (col < 0 || col >= static_cast<int>(sizeof(kLanguages) / sizeof(kLanguages[0])))
        return -1;
    return col;
}

void SettingsScreen::moveLanguageHorizontal(int delta) {
    if (focusRow_ == 1) {
        buttonFocusCol_ += delta;
        if (buttonFocusCol_ < 0)
            buttonFocusCol_ = 1;
        if (buttonFocusCol_ > 1)
            buttonFocusCol_ = 0;
        return;
    }

    int col = languageFocusCol_;
    const int count = static_cast<int>(sizeof(kLanguages) / sizeof(kLanguages[0]));
    for (int i = 0; i < count; ++i) {
        col += delta;
        if (col < 0)
            col = count - 1;
        if (col >= count)
            col = 0;
        int index = languageIndex(col);
        if (index >= 0) {
            languageFocusCol_ = col;
            selectedLanguage_ = kLanguages[index].language;
            return;
        }
    }
}

void SettingsScreen::moveVertical(int delta) {
    if (delta == 0)
        return;

    if (focusRow_ == 1) {
        if (delta < 0)
            focusRow_ = 0;
        return;
    }

    if (delta > 0) {
        focusRow_ = 1;
        buttonFocusCol_ = 0;
        return;
    }
}

SettingsAction SettingsScreen::handleInput(uint64_t kDown, const TouchTap* tap) {
    if (!open_)
        return SettingsAction::None;

    const int cardX = 40;
    const int cardY = 70;
    const int cardW = theme::SCREEN_W - 80;
    const int x = cardX + 24;
    const int gridY = cardY + ui::kPanelTitleBarH + 12 + 34 + 24;
    const int gridCols = static_cast<int>(sizeof(kLanguages) / sizeof(kLanguages[0]));
    const int gap = 10;
    const int cellW = (cardW - 48 - gap * (gridCols - 1)) / gridCols;
    const int cellH = 54;
    const int buttonY = gridY + cellH + 24;
    const int buttonW = 200;
    const int buttonH = 44;
    const int buttonGap = 18;

    if (tap && tap->active) {
        if (ui::panelCloseButtonHit(cardX, cardY, cardW, tap->x, tap->y))
            return SettingsAction::Close;

        for (int col = 0; col < gridCols; ++col) {
            int cellX = x + col * (cellW + gap);
            if (pointInRect(tap, cellX, gridY, cellW, cellH)) {
                focusRow_ = 0;
                languageFocusCol_ = col;
                selectedLanguage_ = kLanguages[col].language;
                return SettingsAction::None;
            }
        }

        if (pointInRect(tap, x, buttonY, buttonW, buttonH)) {
            focusRow_ = 1;
            buttonFocusCol_ = 0;
            return SettingsAction::Close;
        }
        if (pointInRect(tap, x + buttonW + buttonGap, buttonY, buttonW, buttonH)) {
            focusRow_ = 1;
            buttonFocusCol_ = 1;
            return SettingsAction::Save;
        }
    }

    if (kDown & HidNpadButton_B)
        return SettingsAction::Close;
    if (kDown & HidNpadButton_Plus)
        return SettingsAction::Save;

    if (kDown & HidNpadButton_AnyUp)
        moveVertical(-1);
    if (kDown & HidNpadButton_AnyDown)
        moveVertical(1);
    if (kDown & HidNpadButton_AnyLeft)
        moveLanguageHorizontal(-1);
    if (kDown & HidNpadButton_AnyRight)
        moveLanguageHorizontal(1);

    if (kDown & HidNpadButton_A) {
        if (focusRow_ == 1)
            return buttonFocusCol_ == 0 ? SettingsAction::Close : SettingsAction::Save;

        int index = languageIndex(languageFocusCol_);
        if (index >= 0)
            selectedLanguage_ = kLanguages[index].language;
    }

    return SettingsAction::None;
}

void SettingsScreen::render(Renderer& renderer, FontManager& fm, const I18n& i18n) const {
    if (!open_)
        return;

    renderer.drawRectFilled(0, 0, theme::SCREEN_W, theme::SCREEN_H, theme::BG);

    const int cardX = 40;
    const int cardY = 70;
    const int cardW = theme::SCREEN_W - 80;
    const int cardH = theme::SCREEN_H - 140;

    renderer.drawRoundedRectFilled(cardX, cardY, cardW, cardH, 16, theme::MENU_BG);
    renderer.drawRoundedRect(cardX, cardY, cardW, cardH, 16, theme::MENU_BORDER);

    ui::drawPanelTitleBar(renderer, fm, cardX, cardY, cardW, i18n.t("menu.settings"), true, false);

    const int x = cardX + 24;
    int y = cardY + ui::kPanelTitleBarH + 12;

    std::string hints = "+:";
    hints += i18n.t("settings.save");
    hints += "  B:";
    hints += i18n.t("footer.b");
    fm.drawText(renderer.sdl(), hints.c_str(), x, y, theme::FONT_SIZE_SMALL, theme::TEXT_SECONDARY);
    y += 34;

    fm.drawText(renderer.sdl(), i18n.t("settings.language"), x, y, theme::FONT_SIZE_SMALL,
                theme::TEXT_SECONDARY);
    y += 24;

    const int gridCols = static_cast<int>(sizeof(kLanguages) / sizeof(kLanguages[0]));
    const int gap = 10;
    const int cellW = (cardW - 48 - gap * (gridCols - 1)) / gridCols;
    const int cellH = 54;

    for (int col = 0; col < gridCols; ++col) {
        const LanguageCell& cell = kLanguages[col];
        const bool active = cell.language == selectedLanguage_;
        const bool focused = focusRow_ == 0 && languageFocusCol_ == col;

        SDL_Color bg = theme::SURFACE;
        SDL_Color border = theme::DIVIDER;
        SDL_Color text = theme::TEXT;

        if (active) {
            bg = withAlpha(theme::PRIMARY, focused ? 0xff : 0x66);
            border = focused ? theme::PRIMARY : theme::PRIMARY_DIM;
            text = focused ? theme::ON_PRIMARY : theme::PRIMARY;
        } else if (focused) {
            bg = theme::SURFACE_HOVER;
            border = theme::PRIMARY;
        }

        int cellX = x + col * (cellW + gap);
        int cellY = y;
        renderer.drawRoundedRectFilled(cellX, cellY, cellW, cellH, 10, bg);
        renderer.drawRoundedRect(cellX, cellY, cellW, cellH, 10, border);
        fm.drawTextEllipsis(renderer.sdl(), cell.label, cellX + 10,
                            cellY + (cellH - theme::FONT_SIZE_SMALL) / 2,
                            theme::FONT_SIZE_SMALL, text, cellW - 20);
    }

    const int buttonY = y + cellH + 24;
    const int buttonW = 200;
    const int buttonH = 44;
    const int buttonGap = 18;
    struct ButtonCell {
        const char* label;
        bool        focused;
        int         x;
    };
    const ButtonCell buttons[] = {
        {i18n.t("settings.discard"), focusRow_ == 1 && buttonFocusCol_ == 0, x},
        {i18n.t("settings.save"), focusRow_ == 1 && buttonFocusCol_ == 1, x + buttonW + buttonGap},
    };
    for (const auto& cell : buttons) {
        SDL_Color bg = cell.focused ? theme::PRIMARY_DIM : theme::SURFACE;
        SDL_Color border = cell.focused ? theme::PRIMARY : theme::DIVIDER;
        SDL_Color text = cell.focused ? theme::ON_PRIMARY : theme::TEXT;
        renderer.drawRoundedRectFilled(cell.x, buttonY, buttonW, buttonH, 10, bg);
        renderer.drawRoundedRect(cell.x, buttonY, buttonW, buttonH, 10, border);
        fm.drawText(renderer.sdl(), cell.label, cell.x + 16,
                    buttonY + (buttonH - theme::FONT_SIZE_ITEM) / 2,
                    theme::FONT_SIZE_ITEM, text);
    }
}

} // namespace xplore
