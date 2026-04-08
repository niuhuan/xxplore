#include "ui/websocket_installer_screen.hpp"
#include "fs/fs_api.hpp"
#include "i18n/i18n.hpp"
#include "ui/font_manager.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "util/screen_awake.hpp"
#include <algorithm>
#include <string>
#include <utility>
#include <vector>
#include <switch.h>

namespace xplore {

namespace {

void drawProgressBar(Renderer& renderer, int x, int y, int w, int h, float progress) {
    if (progress < 0.0f)
        progress = 0.0f;
    if (progress > 1.0f)
        progress = 1.0f;
    renderer.drawRoundedRectFilled(x, y, w, h, 8, theme::SURFACE);
    int fillW = static_cast<int>(static_cast<float>(w) * progress);
    if (progress > 0.0f && fillW <= 0)
        fillW = 1;
    if (fillW > w)
        fillW = w;
    if (fillW > 0)
        renderer.drawRoundedRectFilled(x, y, fillW, h, 8, theme::PRIMARY);
    renderer.drawRoundedRect(x, y, w, h, 8, theme::DIVIDER);
}

SDL_Color withAlpha(SDL_Color color, Uint8 alpha) {
    color.a = alpha;
    return color;
}

} // namespace

void WebSocketInstallerScreen::open(const I18n& i18n) {
    close();
    WebSocketInstallerServer::TextMap textMap = {
        {"page_title", i18n.t("web.page_title")},
        {"page_sub", i18n.t("web.page_sub")},
        {"page_label_connection", i18n.t("web.page.label_connection")},
        {"page_label_status", i18n.t("web.page.label_status")},
        {"page_label_hint", i18n.t("web.page.label_hint")},
        {"page_hint_idle", i18n.t("web.page.hint_idle")},
        {"page_button_add_files", i18n.t("web.page.button_add_files")},
        {"page_button_clear_queue", i18n.t("web.page.button_clear_queue")},
        {"page_button_start_install", i18n.t("web.page.button_start_install")},
        {"page_label_current_package", i18n.t("web.page.label_current_package")},
        {"page_label_overall_progress", i18n.t("web.page.label_overall_progress")},
        {"page_col_id", i18n.t("web.page.col_id")},
        {"page_col_name", i18n.t("web.page.col_name")},
        {"page_col_size", i18n.t("web.page.col_size")},
        {"page_conn_http_ready", i18n.t("web.page.connection_http_ready")},
        {"page_conn_connecting", i18n.t("web.page.connection_connecting")},
        {"page_conn_active", i18n.t("web.page.connection_active")},
        {"page_status_waiting", i18n.t("web.page.status_waiting")},
        {"page_status_connecting", i18n.t("web.page.status_connecting")},
        {"page_status_manifest", i18n.t("web.page.status_sending_manifest")},
        {"page_status_connect_failed", i18n.t("web.page.status_connect_failed")},
        {"page_status_connection_lost", i18n.t("web.page.status_connection_lost")},
        {"page_status_completed", i18n.t("web.page.status_completed")},
        {"page_status_failed", i18n.t("web.page.status_failed")},
        {"page_status_error", i18n.t("web.page.status_error")},
        {"page_hint_connecting", i18n.t("web.page.hint_connecting")},
        {"page_hint_active", i18n.t("web.page.hint_active")},
        {"page_hint_connect_failed", i18n.t("web.page.hint_connect_failed")},
        {"page_no_queued_files", i18n.t("web.page.no_queued_files")},
        {"page_log_ws_connected", i18n.t("web.page.log_ws_connected")},
        {"page_log_ws_closed", i18n.t("web.page.log_ws_closed")},
        {"page_log_ws_closed_during", i18n.t("web.page.log_ws_closed_during")},
        {"page_log_ws_error", i18n.t("web.page.log_ws_error")},
        {"page_log_skip_unsupported", i18n.t("web.page.log_skip_unsupported")},
        {"page_log_install_request", i18n.t("web.page.log_install_request")},
        {"page_log_missing_file", i18n.t("web.page.log_missing_file")},
        {"page_log_unexpected_binary", i18n.t("web.page.log_unexpected_binary")},
        {"page_log_ns_hello", i18n.t("web.page.log_ns_hello")},
        {"page_log_ns_success", i18n.t("web.page.log_ns_success")},
        {"page_log_ns_failed", i18n.t("web.page.log_ns_failed")},
        {"page_log_ns_error", i18n.t("web.page.log_ns_error")},
        {"status_waiting_browser", i18n.t("websocket_installer.waiting_browser")},
        {"status_browser_connected", i18n.t("websocket_installer.status_browser_connected")},
        {"status_installing", i18n.t("websocket_installer.status_installing")},
        {"status_preparing_install", i18n.t("websocket_installer.status_preparing_install")},
        {"status_install_completed", i18n.t("websocket_installer.status_install_completed")},
        {"status_install_failed", i18n.t("websocket_installer.status_install_failed")},
        {"status_install_aborted", i18n.t("websocket_installer.status_install_aborted")},
        {"error_conflict_abort", i18n.t("websocket_installer.error_conflict_abort")},
        {"error_browser_disconnected", i18n.t("websocket_installer.error_browser_disconnected")},
        {"error_session_closed", i18n.t("websocket_installer.error_session_closed")},
        {"error_browser_not_connected", i18n.t("websocket_installer.error_browser_not_connected")},
        {"error_send_read_request", i18n.t("websocket_installer.error_send_read_request")},
    };
    server_.setTextMap(std::move(textMap));
    util::acquireScreenAwake();
    open_ = true;
    focusRow_ = 1;
    targetFocusCol_ = 1;
    buttonFocusCol_ = 1;
}

void WebSocketInstallerScreen::close() {
    if (!open_)
        return;
    server_.stop();
    util::releaseScreenAwake();
    open_ = false;
}

WebSocketInstallerAction WebSocketInstallerScreen::handleInput(uint64_t kDown) {
    if (!open_)
        return WebSocketInstallerAction::None;

    if (server_.isInstalling())
        return WebSocketInstallerAction::None;

    if (!server_.isRunning()) {
        if (kDown & HidNpadButton_B)
            return WebSocketInstallerAction::Close;

        if (kDown & (HidNpadButton_AnyUp | HidNpadButton_AnyDown))
            focusRow_ = 1 - focusRow_;

        if (kDown & HidNpadButton_AnyLeft) {
            if (focusRow_ == 0) {
                if (targetFocusCol_ > 0)
                    targetFocusCol_--;
            } else if (buttonFocusCol_ > 0) {
                buttonFocusCol_--;
            }
        }
        if (kDown & HidNpadButton_AnyRight) {
            if (focusRow_ == 0) {
                if (targetFocusCol_ < 1)
                    targetFocusCol_++;
            } else if (buttonFocusCol_ < 1) {
                buttonFocusCol_++;
            }
        }

        if (kDown & HidNpadButton_A) {
            if (focusRow_ == 1 && buttonFocusCol_ == 0)
                return WebSocketInstallerAction::Close;
            if (focusRow_ == 1 && buttonFocusCol_ == 1) {
                server_.start(targetFocusCol_ == 0 ? WebInstallTarget::Nand : WebInstallTarget::SdCard);
            }
        }
        return WebSocketInstallerAction::None;
    }

    if (kDown & (HidNpadButton_B | HidNpadButton_Plus))
        return WebSocketInstallerAction::Close;
    return WebSocketInstallerAction::None;
}

void WebSocketInstallerScreen::render(Renderer& renderer, FontManager& fm, const I18n& i18n) const {
    if (!open_)
        return;

    renderer.drawRectFilled(0, 0, theme::SCREEN_W, theme::SCREEN_H, theme::BG);

    const int cardX = 40;
    const int cardY = 40;
    const int cardW = theme::SCREEN_W - 80;
    const int cardH = theme::SCREEN_H - 80;

    renderer.drawRoundedRectFilled(cardX, cardY, cardW, cardH, 16, theme::MENU_BG);
    renderer.drawRoundedRect(cardX, cardY, cardW, cardH, 16, theme::MENU_BORDER);

    const int x = cardX + 24;
    int y = cardY + 20;

    fm.drawText(renderer.sdl(), i18n.t("websocket_installer.title"), x, y, theme::FONT_SIZE_TITLE,
                theme::PRIMARY);
    y += 34;

    if (!server_.isRunning()) {
        fm.drawText(renderer.sdl(), i18n.t("websocket_installer.description"),
                    x, y, theme::FONT_SIZE_SMALL, theme::TEXT_SECONDARY);
        y += 32;
        fm.drawText(renderer.sdl(), i18n.t("websocket_installer.close_hint"), x, y,
                    theme::FONT_SIZE_SMALL, theme::TEXT_SECONDARY);
        y += 36;

        const int targetW = 180;
        const int targetH = 42;
        const int gap = 16;
        struct TargetCell {
            const char* label;
            bool active;
            bool focused;
            int x;
        };
        const TargetCell targets[] = {
            {i18n.t("installer.target_nand"), targetFocusCol_ == 0,
             focusRow_ == 0 && targetFocusCol_ == 0, x},
            {i18n.t("installer.target_sd"), targetFocusCol_ == 1,
             focusRow_ == 0 && targetFocusCol_ == 1, x + targetW + gap},
        };
        for (const auto& cell : targets) {
            SDL_Color bg = theme::SURFACE;
            SDL_Color border = theme::DIVIDER;
            SDL_Color text = theme::TEXT;
            if (cell.active) {
                bg = withAlpha(theme::PRIMARY, cell.focused ? 0xff : 0x66);
                border = cell.focused ? theme::PRIMARY : theme::PRIMARY_DIM;
                text = cell.focused ? theme::ON_PRIMARY : theme::PRIMARY;
            } else if (cell.focused) {
                bg = theme::SURFACE_HOVER;
                border = theme::PRIMARY;
            }
            renderer.drawRoundedRectFilled(cell.x, y, targetW, targetH, 10, bg);
            renderer.drawRoundedRect(cell.x, y, targetW, targetH, 10, border);
            fm.drawText(renderer.sdl(), cell.label, cell.x + 16,
                        y + (targetH - theme::FONT_SIZE_ITEM) / 2, theme::FONT_SIZE_ITEM, text);
        }
        y += targetH + 28;

        const int buttonY = y;
        struct ButtonCell {
            const char* label;
            bool focused;
            int x;
        };
        const ButtonCell buttons[] = {
            {i18n.t("installer.cancel"), focusRow_ == 1 && buttonFocusCol_ == 0, x},
            {i18n.t("websocket_installer.start_server"), focusRow_ == 1 && buttonFocusCol_ == 1,
             x + targetW + gap},
        };
        for (const auto& cell : buttons) {
            SDL_Color bg = cell.focused ? theme::PRIMARY_DIM : theme::SURFACE;
            SDL_Color border = cell.focused ? theme::PRIMARY : theme::DIVIDER;
            SDL_Color text = cell.focused ? theme::ON_PRIMARY : theme::TEXT;
            renderer.drawRoundedRectFilled(cell.x, buttonY, targetW, targetH, 10, bg);
            renderer.drawRoundedRect(cell.x, buttonY, targetW, targetH, 10, border);
            fm.drawText(renderer.sdl(), cell.label, cell.x + 16,
                        buttonY + (targetH - theme::FONT_SIZE_ITEM) / 2,
                        theme::FONT_SIZE_ITEM, text);
        }
        return;
    }

    std::string targetLine = std::string(i18n.t("installer.target")) + ": " +
                             (server_.target() == WebInstallTarget::Nand
                                  ? i18n.t("installer.target_nand")
                                  : i18n.t("installer.target_sd"));
    fm.drawText(renderer.sdl(), targetLine.c_str(), x, y, theme::FONT_SIZE_SMALL,
                theme::TEXT_SECONDARY);
    y += 24;

    std::string urlLine = server_.url();
    fm.drawTextEllipsis(renderer.sdl(), urlLine.c_str(), x, y, theme::FONT_SIZE_ITEM,
                        theme::TEXT, cardW - 48);
    y += 36;

    std::string summary = i18n.t("websocket_installer.waiting_browser");
    if (server_.itemCount() > 0) {
        summary = i18n.t("installer.summary_prefix");
        summary += " ";
        summary += std::to_string(server_.itemCount());
        summary += " ";
        summary += i18n.t(server_.itemCount() == 1 ? "installer.file_single"
                                                   : "installer.file_plural");
        summary += ", ";
        summary += fs::formatSize(server_.totalBytes());
    }
    fm.drawTextEllipsis(renderer.sdl(), summary.c_str(), x, y, theme::FONT_SIZE_ITEM,
                        theme::TEXT, cardW - 48);
    y += 34;

    std::string status = server_.status();
    fm.drawTextEllipsis(renderer.sdl(), status.c_str(), x, y, theme::FONT_SIZE_SMALL,
                        theme::TEXT_SECONDARY, cardW - 48);
    y += 26;

    int logY = y;
    int logH = server_.isInstalling() ? 260 : 360;
    renderer.drawRoundedRectFilled(x, logY, cardW - 48, logH, 10, theme::BG);
    renderer.drawRoundedRect(x, logY, cardW - 48, logH, 10, theme::DIVIDER);
    fm.drawText(renderer.sdl(), i18n.t("installer.console"), x + 12, logY + 10,
                theme::FONT_SIZE_SMALL,
                theme::TEXT_SECONDARY);

    std::vector<std::string> logs = server_.logs();
    int lineY = logY + 36;
    int lineH = fm.fontHeight(theme::FONT_SIZE_SMALL) + 4;
    int maxVisible = std::max(1, (logH - 48) / lineH);
    int start = 0;
    if (static_cast<int>(logs.size()) > maxVisible)
        start = static_cast<int>(logs.size()) - maxVisible;
    for (int i = start; i < static_cast<int>(logs.size()); ++i) {
        fm.drawTextEllipsis(renderer.sdl(), logs[static_cast<size_t>(i)].c_str(), x + 12, lineY,
                            theme::FONT_SIZE_SMALL, theme::TEXT, cardW - 72);
        lineY += lineH;
    }

    if (server_.isInstalling()) {
        int barY = logY + logH + 24;
        std::string currentItem = server_.currentItem();
        if (currentItem.empty())
            currentItem = i18n.t("websocket_installer.preparing");
        fm.drawTextEllipsis(renderer.sdl(), currentItem.c_str(), x, barY,
                            theme::FONT_SIZE_SMALL, theme::TEXT_SECONDARY, cardW - 48);
        barY += 22;
        drawProgressBar(renderer, x, barY, cardW - 48, 18, server_.currentProgress());
        barY += 36;
        fm.drawText(renderer.sdl(), i18n.t("installer.total_progress"), x, barY,
                    theme::FONT_SIZE_SMALL, theme::TEXT_SECONDARY);
        barY += 22;
        drawProgressBar(renderer, x, barY, cardW - 48, 18, server_.totalProgress());
    } else {
        fm.drawText(renderer.sdl(), i18n.t("websocket_installer.close_server_hint"),
                    x, cardY + cardH - 32,
                    theme::FONT_SIZE_SMALL, theme::TEXT_SECONDARY);
    }
}

} // namespace xplore
