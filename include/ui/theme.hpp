#pragma once
#include <SDL.h>

namespace xplore {
/// Theme constants ported from plorex-js (colors.ts + layout.ts).
namespace theme {

// --- Colors ---
constexpr SDL_Color BG              = {0x1e, 0x1e, 0x2e, 0xff};
constexpr SDL_Color SURFACE         = {0x2a, 0x2a, 0x3c, 0xff};
constexpr SDL_Color SURFACE_HOVER   = {0x35, 0x35, 0x48, 0xff};
constexpr SDL_Color PRIMARY         = {0x60, 0xa5, 0xfa, 0xff};
constexpr SDL_Color PRIMARY_DIM     = {0x3b, 0x82, 0xf6, 0xff};
constexpr SDL_Color ON_PRIMARY      = {0xff, 0xff, 0xff, 0xff};
constexpr SDL_Color TEXT            = {0xe0, 0xe0, 0xe0, 0xff};
constexpr SDL_Color TEXT_SECONDARY  = {0x88, 0x88, 0x98, 0xff};
constexpr SDL_Color TEXT_DISABLED   = {0x55, 0x55, 0x68, 0xff};
constexpr SDL_Color DIVIDER         = {0x3a, 0x3a, 0x4c, 0xff};
constexpr SDL_Color DANGER          = {0xef, 0x44, 0x44, 0xff};
constexpr SDL_Color MASK_OVERLAY    = {0x00, 0x00, 0x00, 0x8c};
constexpr SDL_Color CURSOR_ROW      = {0x60, 0xa5, 0xfa, 0x2e};
constexpr SDL_Color SELECTED_ROW    = {0x60, 0xa5, 0xfa, 0x1f};
constexpr SDL_Color SELECTED_BAR    = {0x60, 0xa5, 0xfa, 0xff};
constexpr SDL_Color CHECKBOX_BORDER = {0x60, 0xa5, 0xfa, 0xff};
constexpr SDL_Color CHECKBOX_FILL   = {0x60, 0xa5, 0xfa, 0xff};
constexpr SDL_Color HEADER_BG       = {0x25, 0x25, 0x36, 0xff};
constexpr SDL_Color BREADCRUMB_TEXT = {0xa0, 0xa0, 0xb8, 0xff};
constexpr SDL_Color TOAST_BG            = {0x12, 0x12, 0x1c, 0xf0};
constexpr SDL_Color TOAST_TEXT          = {0xf8, 0xd7, 0xda, 0xff};
constexpr SDL_Color TOAST_ERROR_BORDER  = {0xef, 0x44, 0x44, 0xff};
constexpr SDL_Color TOAST_SUCCESS_BORDER= {0x22, 0xc5, 0x5e, 0xff};
constexpr SDL_Color TOAST_WARNING_BORDER= {0xf5, 0x9e, 0x0b, 0xff};
constexpr SDL_Color TOAST_INFO_BORDER   = {0x60, 0xa5, 0xfa, 0xff};
constexpr SDL_Color MENU_OVERLAY    = {0x00, 0x00, 0x00, 0x99};
/// Light scrim over header+file panels only (footer stays readable; list remains visible).
constexpr SDL_Color MENU_SCRIM_CONTENT = {0x00, 0x00, 0x00, 0x55};
constexpr SDL_Color MENU_BG         = {0x2a, 0x2a, 0x3c, 0xff};
constexpr SDL_Color MENU_BORDER     = {0x3a, 0x3a, 0x4c, 0xff};
constexpr SDL_Color MENU_ITEM_TEXT  = {0xe0, 0xe0, 0xe0, 0xff};
constexpr SDL_Color MENU_DIM_EXTRA  = {0x00, 0x00, 0x00, 0x66}; ///< Extra dim on modals only

// --- Screen & layout ---
constexpr int SCREEN_W         = 1280;
constexpr int SCREEN_H         = 720;
constexpr int HEADER_H         = 50;
constexpr int FOOTER_H         = 37;   ///< Bottom button-tips bar height
constexpr int ITEM_H           = 56;   ///< Row height for file items / list items
constexpr int INACTIVE_PANEL_W = 50;
constexpr int ACTIVE_PANEL_W   = SCREEN_W - INACTIVE_PANEL_W;
constexpr int PANEL_CONTENT_H  = SCREEN_H - HEADER_H - FOOTER_H;
constexpr int ICON_SIZE        = 32;   ///< File-type icon dimensions (square)
constexpr int CHECKBOX_SIZE    = 24;
constexpr int PADDING          = 12;
constexpr int PADDING_SM       = 8;

// --- Font sizes ---
constexpr int FONT_SIZE_TITLE  = 20;
constexpr int FONT_SIZE_ITEM   = 18;
constexpr int FONT_SIZE_SMALL  = 14;
constexpr int FONT_SIZE_FOOTER = 16;   ///< Footer button-tips font size

// --- Menu ---
constexpr int MENU_W           = 400;
constexpr int MENU_ITEM_H      = 48;
constexpr int MENU_RADIUS      = 12;
constexpr int MENU_PADDING     = 8;
/// Bottom sheet main menu (4-column grid)
constexpr int MENU_SHEET_MARGIN_X = 20;
constexpr int MENU_SHEET_TITLE_H  = 34;
constexpr int MENU_SHEET_CONTEXT_H = 56;
constexpr int MENU_SHEET_CELL_H   = 50;
constexpr int MENU_SHEET_ROWS     = 5; ///< Action rows under context
constexpr int MENU_SHEET_ANIM_MS  = 220;

// --- Toast ---
constexpr int TOAST_W        = 600;
constexpr int TOAST_H        = 80;
constexpr int TOAST_MARGIN   = 20;
constexpr int TOAST_BORDER_W = 3;
constexpr int TOAST_RADIUS   = 10;

// --- Animation ---
constexpr int   ANIM_DURATION_MS = 250;
constexpr float ANIM_DURATION_F  = 250.0f;

} // namespace theme
} // namespace xplore
