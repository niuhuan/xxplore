#include "ui/network_drive_form.hpp"
#include "i18n/i18n.hpp"
#include "swkbd_input.hpp"
#include "ui/font_manager.hpp"
#include "ui/panel_chrome.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "util/app_config.hpp"
#include <cstdio>
#include <switch.h>

namespace xxplore {

namespace {

SDL_Color withAlpha(SDL_Color color, Uint8 alpha) {
    color.a = alpha;
    return color;
}

} // namespace

void NetworkDriveForm::openNew() {
    open_ = true;
    editing_ = false;
    config_ = {};
    config_.id = config::generateDriveId();
    config_.type = config::NetworkDriveType::WebDAV;
    focusRow_ = kRowName;
    buttonFocusCol_ = 0;
}

void NetworkDriveForm::openEdit(const config::NetworkDriveConfig& existing) {
    open_ = true;
    editing_ = true;
    config_ = existing;
    focusRow_ = kRowName;
    buttonFocusCol_ = 0;
}

void NetworkDriveForm::close() {
    open_ = false;
}

void NetworkDriveForm::moveVertical(int delta) {
    focusRow_ += delta;
    if (focusRow_ < kRowName)
        focusRow_ = kRowButtons;
    if (focusRow_ > kRowButtons)
        focusRow_ = kRowName;
}

void NetworkDriveForm::moveHorizontal(int delta) {
    if (focusRow_ == kRowType) {
        // Toggle type
        config_.type = (config_.type == config::NetworkDriveType::WebDAV)
                           ? config::NetworkDriveType::SMB2
                           : config::NetworkDriveType::WebDAV;
    } else if (focusRow_ == kRowButtons) {
        buttonFocusCol_ += delta;
        if (buttonFocusCol_ < 0) buttonFocusCol_ = 1;
        if (buttonFocusCol_ > 1) buttonFocusCol_ = 0;
    }
}

void NetworkDriveForm::activateRow(const I18n& i18n) {
    char buf[512] = {0};

    switch (focusRow_) {
    case kRowName:
        if (swkbdTextInput(i18n.t("network_form.name"), i18n.t("network_form.name_hint"),
                           config_.name.c_str(), buf, sizeof(buf)))
            config_.name = buf;
        break;
    case kRowType:
        config_.type = (config_.type == config::NetworkDriveType::WebDAV)
                           ? config::NetworkDriveType::SMB2
                           : config::NetworkDriveType::WebDAV;
        break;
    case kRowAddress: {
        const char* hint = config_.type == config::NetworkDriveType::WebDAV
                               ? "http://192.168.1.1/dav"
                               : "192.168.1.1/share";
        if (swkbdTextInput(i18n.t("network_form.address"), hint,
                           config_.address.c_str(), buf, sizeof(buf)))
            config_.address = buf;
        break;
    }
    case kRowUsername:
        if (swkbdTextInput(i18n.t("network_form.username"), i18n.t("network_form.username_hint"),
                           config_.username.c_str(), buf, sizeof(buf)))
            config_.username = buf;
        break;
    case kRowPassword:
        if (swkbdTextInput(i18n.t("network_form.password"), i18n.t("network_form.password_hint"),
                           config_.password.c_str(), buf, sizeof(buf)))
            config_.password = buf;
        break;
    default:
        break;
    }
}

NetworkDriveFormAction NetworkDriveForm::handleInput(uint64_t kDown, const I18n& i18n,
                                                     const TouchTap* tap) {
    if (!open_)
        return NetworkDriveFormAction::None;

    const int cardX = 80;
    const int cardY = 50;
    const int cardW = theme::SCREEN_W - 160;
    const int x = cardX + 24;
    const int rowH = 52;
    const int labelW = 160;
    const int valueX = x + labelW;
    const int valueW = cardW - 48 - labelW;
    const int typeCellGap = 12;
    const int typeCellW = (valueW - typeCellGap) / 2;
    const int rowsTopY = cardY + ui::kPanelTitleBarH + 12 + 30;
    const int buttonsY = rowsTopY + (rowH + 8) * 5 + 8;
    const int buttonW = 200;
    const int buttonH = 44;
    const int buttonGap = 18;

    if (tap && tap->active) {
        if (ui::panelCloseButtonHit(cardX, cardY, cardW, tap->x, tap->y))
            return NetworkDriveFormAction::Close;

        int rowY = rowsTopY;
        for (int row = kRowName; row <= kRowPassword; ++row) {
            if (row == kRowType) {
                if (pointInRect(tap, valueX, rowY, typeCellW, rowH)) {
                    focusRow_ = kRowType;
                    config_.type = config::NetworkDriveType::WebDAV;
                    return NetworkDriveFormAction::None;
                }
                if (pointInRect(tap, valueX + typeCellW + typeCellGap, rowY, typeCellW, rowH)) {
                    focusRow_ = kRowType;
                    config_.type = config::NetworkDriveType::SMB2;
                    return NetworkDriveFormAction::None;
                }
            } else if (pointInRect(tap, x, rowY, cardW - 48, rowH)) {
                focusRow_ = row;
                activateRow(i18n);
                return NetworkDriveFormAction::None;
            }
            rowY += rowH + 8;
        }

        if (pointInRect(tap, x, buttonsY, buttonW, buttonH)) {
            focusRow_ = kRowButtons;
            buttonFocusCol_ = 0;
            return NetworkDriveFormAction::Close;
        }
        if (pointInRect(tap, x + buttonW + buttonGap, buttonsY, buttonW, buttonH)) {
            focusRow_ = kRowButtons;
            buttonFocusCol_ = 1;
            return NetworkDriveFormAction::Save;
        }
    }

    if (kDown & HidNpadButton_B)
        return NetworkDriveFormAction::Close;

    if (kDown & HidNpadButton_AnyUp)   moveVertical(-1);
    if (kDown & HidNpadButton_AnyDown) moveVertical(1);
    if (kDown & HidNpadButton_AnyLeft)  moveHorizontal(-1);
    if (kDown & HidNpadButton_AnyRight) moveHorizontal(1);

    if (kDown & HidNpadButton_A) {
        if (focusRow_ == kRowButtons) {
            if (buttonFocusCol_ == 0)
                return NetworkDriveFormAction::Close;
            else
                return NetworkDriveFormAction::Save;
        }
        activateRow(i18n);
    }

    return NetworkDriveFormAction::None;
}

void NetworkDriveForm::render(Renderer& renderer, FontManager& fm, const I18n& i18n) const {
    if (!open_)
        return;

    using namespace theme;

    renderer.drawRectFilled(0, 0, SCREEN_W, SCREEN_H, BG);

    const int cardX = 80;
    const int cardY = 50;
    const int cardW = SCREEN_W - 160;
    const int cardH = SCREEN_H - 100;

    renderer.drawRoundedRectFilled(cardX, cardY, cardW, cardH, 16, MENU_BG);
    renderer.drawRoundedRect(cardX, cardY, cardW, cardH, 16, MENU_BORDER);

    const char* title = editing_ ? i18n.t("network_form.title_edit")
                                 : i18n.t("network_form.title_add");
    ui::drawPanelTitleBar(renderer, fm, cardX, cardY, cardW, title, true, false);

    const int x = cardX + 24;
    int y = cardY + ui::kPanelTitleBarH + 12;

    // Hints line
    std::string hints = "B:";
    hints += i18n.t("footer.b");
    fm.drawText(renderer.sdl(), hints.c_str(), x, y, FONT_SIZE_SMALL, TEXT_SECONDARY);
    y += 30;

    // Row rendering helper
    const int rowH = 52;
    const int labelW = 160;
    const int valueX = x + labelW;
    const int valueW = cardW - 48 - labelW;

    struct Row {
        const char* label;
        std::string value;
        int rowIdx;
    };

    auto maskPassword = [](const std::string& pw) -> std::string {
        if (pw.empty()) return "";
        return std::string(pw.size(), '*');
    };

    const char* addressPlaceholder = config_.type == config::NetworkDriveType::WebDAV
                                         ? "http://192.168.1.1/dav"
                                         : "192.168.1.1/share";

    Row rows[] = {
        {i18n.t("network_form.name"),     config_.name.empty() ? i18n.t("network_form.name_hint") : config_.name, kRowName},
        {i18n.t("network_form.type"),     "", kRowType},
        {i18n.t("network_form.address"),  config_.address.empty() ? addressPlaceholder : config_.address, kRowAddress},
        {i18n.t("network_form.username"), config_.username.empty() ? i18n.t("network_form.username_hint") : config_.username, kRowUsername},
        {i18n.t("network_form.password"), config_.password.empty() ? i18n.t("network_form.password_hint") : maskPassword(config_.password), kRowPassword},
    };

    for (const auto& row : rows) {
        bool focused = focusRow_ == row.rowIdx;
        SDL_Color rowBg = focused ? SURFACE_HOVER : SURFACE;
        SDL_Color rowBorder = focused ? PRIMARY : DIVIDER;

        renderer.drawRoundedRectFilled(x, y, cardW - 48, rowH, 8, rowBg);
        renderer.drawRoundedRect(x, y, cardW - 48, rowH, 8, rowBorder);

        int textY = y + (rowH - FONT_SIZE_ITEM) / 2;
        fm.drawText(renderer.sdl(), row.label, x + 12, textY, FONT_SIZE_ITEM, TEXT_SECONDARY);

        bool isEmpty = (row.rowIdx == kRowName && config_.name.empty()) ||
                       (row.rowIdx == kRowAddress && config_.address.empty()) ||
                       (row.rowIdx == kRowUsername && config_.username.empty()) ||
                       (row.rowIdx == kRowPassword && config_.password.empty());
        SDL_Color valColor = isEmpty ? TEXT_DISABLED : TEXT;

        if (row.rowIdx == kRowType) {
            const int typeCellGap = 12;
            const int typeCellW = (valueW - typeCellGap) / 2;
            struct TypeCell {
                const char* label;
                bool active;
                int x;
            };
            const TypeCell typeCells[] = {
                {"WebDAV", config_.type == config::NetworkDriveType::WebDAV, valueX},
                {"SMB2.0", config_.type == config::NetworkDriveType::SMB2,
                 valueX + typeCellW + typeCellGap},
            };
            for (const auto& cell : typeCells) {
                SDL_Color typeBg = SURFACE;
                SDL_Color typeBorder = DIVIDER;
                SDL_Color typeText = TEXT;
                if (cell.active) {
                    typeBg = withAlpha(PRIMARY, focused ? 0xff : 0x66);
                    typeBorder = focused ? PRIMARY : PRIMARY_DIM;
                    typeText = focused ? ON_PRIMARY : PRIMARY;
                } else if (focused) {
                    typeBg = SURFACE_HOVER;
                    typeBorder = PRIMARY;
                }
                renderer.drawRoundedRectFilled(cell.x, y, typeCellW, rowH, 10, typeBg);
                renderer.drawRoundedRect(cell.x, y, typeCellW, rowH, 10, typeBorder);
                fm.drawText(renderer.sdl(), cell.label, cell.x + 16,
                            y + (rowH - FONT_SIZE_ITEM) / 2, FONT_SIZE_ITEM, typeText);
            }
        } else {
            fm.drawTextEllipsis(renderer.sdl(), row.value.c_str(), valueX, textY,
                                FONT_SIZE_ITEM, valColor, valueW);
        }

        y += rowH + 8;
    }

    // Buttons
    y += 8;
    const int buttonW = 200;
    const int buttonH = 44;
    const int buttonGap = 18;

    struct BtnCell {
        const char* label;
        bool focused;
        int bx;
    };
    BtnCell buttons[] = {
        {i18n.t("network_form.back"), focusRow_ == kRowButtons && buttonFocusCol_ == 0, x},
        {i18n.t("network_form.save"), focusRow_ == kRowButtons && buttonFocusCol_ == 1, x + buttonW + buttonGap},
    };
    for (const auto& btn : buttons) {
        SDL_Color bg = btn.focused ? PRIMARY_DIM : SURFACE;
        SDL_Color border = btn.focused ? PRIMARY : DIVIDER;
        SDL_Color text = btn.focused ? ON_PRIMARY : TEXT;
        renderer.drawRoundedRectFilled(btn.bx, y, buttonW, buttonH, 10, bg);
        renderer.drawRoundedRect(btn.bx, y, buttonW, buttonH, 10, border);
        fm.drawText(renderer.sdl(), btn.label, btn.bx + 16,
                    y + (buttonH - FONT_SIZE_ITEM) / 2, FONT_SIZE_ITEM, text);
    }
}

} // namespace xxplore
