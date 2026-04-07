#include <cstdio>
#include <string>
#include <vector>
#include <switch.h>

#include "ui/theme.hpp"
#include "ui/renderer.hpp"
#include "ui/font_manager.hpp"
#include "ui/file_list.hpp"
#include "ui/toast.hpp"

enum { ACTION_NONE = 0, ACTION_SHOW_TOAST };
enum ActivePanel { PANEL_LEFT, PANEL_RIGHT };

static std::vector<xplore::ListItem> buildListItems() {
    std::vector<xplore::ListItem> items;
    items.push_back({"sdmc:", ACTION_NONE});
#ifdef XPLORE_DEBUG
    items.push_back({"Test Toast", ACTION_SHOW_TOAST});
#endif
    return items;
}

int main(int argc, char* argv[]) {
#ifdef XPLORE_DEBUG
    socketInitializeDefault();
    nxlinkStdio();
    printf("Xplore: nxlink connected\n");
#endif

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    xplore::Renderer renderer;
    xplore::FontManager fontManager;
    bool ok = renderer.init();
    if (ok) ok = fontManager.init();

    if (ok) {
        xplore::Toast toast;

        xplore::FileList leftList;
        xplore::FileList rightList;
        leftList.setItems(buildListItems());
        rightList.setItems(buildListItems());

        ActivePanel activePanel = PANEL_LEFT;
        uint32_t lastTick = SDL_GetTicks();

        while (appletMainLoop()) {
            uint32_t now = SDL_GetTicks();
            uint32_t delta = now - lastTick;
            lastTick = now;

            padUpdate(&pad);
            u64 kDown = padGetButtonsDown(&pad);

            if (kDown & HidNpadButton_Plus)
                break;

            // L/R panel switching
            if (kDown & HidNpadButton_L) {
                activePanel = PANEL_LEFT;
                printf("Switch to LEFT panel\n");
            }
            if (kDown & HidNpadButton_R) {
                activePanel = PANEL_RIGHT;
                printf("Switch to RIGHT panel\n");
            }

            // Input goes to active panel only
            xplore::FileList& active = (activePanel == PANEL_LEFT) ? leftList : rightList;

            if (kDown & HidNpadButton_AnyUp)
                active.moveCursorUp();
            if (kDown & HidNpadButton_AnyDown)
                active.moveCursorDown();

            if (kDown & HidNpadButton_A) {
                auto* item = active.getSelectedItem();
                if (item && item->action == ACTION_SHOW_TOAST) {
                    toast.show("List Directory: sdmc:/",
                               "No such device (open dir)");
                    printf("Toast triggered\n");
                }
            }

            toast.update(delta);

            // --- Render ---
            renderer.clear(xplore::theme::BG);

            // Header
            renderer.drawRectFilled(0, 0,
                xplore::theme::SCREEN_W, xplore::theme::HEADER_H,
                xplore::theme::HEADER_BG);
            fontManager.drawText(renderer.sdl(), "Xplore",
                xplore::theme::PADDING,
                (xplore::theme::HEADER_H - xplore::theme::FONT_SIZE_TITLE) / 2,
                xplore::theme::FONT_SIZE_TITLE, xplore::theme::PRIMARY);
            renderer.drawRectFilled(0, xplore::theme::HEADER_H - 1,
                xplore::theme::SCREEN_W, 1, xplore::theme::DIVIDER);

            // Dual panel layout
            int leftW, rightW, rightX;
            if (activePanel == PANEL_LEFT) {
                leftW  = xplore::theme::ACTIVE_PANEL_W;
                rightW = xplore::theme::INACTIVE_PANEL_W;
            } else {
                leftW  = xplore::theme::INACTIVE_PANEL_W;
                rightW = xplore::theme::ACTIVE_PANEL_W;
            }
            rightX = leftW;

            int panelY = xplore::theme::HEADER_H;
            int panelH = xplore::theme::PANEL_CONTENT_H;

            // Left panel
            leftList.render(renderer, fontManager,
                0, panelY, leftW, panelH,
                activePanel == PANEL_LEFT);

            // Inactive mask on left
            if (activePanel != PANEL_LEFT) {
                renderer.drawRectFilled(0, panelY, leftW, panelH,
                    xplore::theme::MASK_OVERLAY);
            }

            // Divider
            renderer.drawRectFilled(rightX - 1, panelY, 2, panelH,
                xplore::theme::DIVIDER);

            // Right panel
            rightList.render(renderer, fontManager,
                rightX, panelY, rightW, panelH,
                activePanel == PANEL_RIGHT);

            // Inactive mask on right
            if (activePanel != PANEL_RIGHT) {
                renderer.drawRectFilled(rightX, panelY, rightW, panelH,
                    xplore::theme::MASK_OVERLAY);
            }

            toast.render(renderer, fontManager);
            renderer.present();
        }
    }

    fontManager.shutdown();
    renderer.shutdown();

#ifdef XPLORE_DEBUG
    socketExit();
#endif
    return 0;
}
