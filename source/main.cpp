#include <cstdio>
#include <string>
#include <vector>
#include <switch.h>

#include "ui/theme.hpp"
#include "ui/renderer.hpp"
#include "ui/font_manager.hpp"
#include "ui/toast.hpp"

struct ListItem {
    std::string label;
    enum Action { NONE, SHOW_TOAST } action;
};

static std::vector<ListItem> buildListItems() {
    std::vector<ListItem> items;
    items.push_back({"sdmc:", ListItem::NONE});
#ifdef XPLORE_DEBUG
    items.push_back({"Test Toast", ListItem::SHOW_TOAST});
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
        auto items = buildListItems();
        int cursor = 0;
        uint32_t lastTick = SDL_GetTicks();

        while (appletMainLoop()) {
            uint32_t now = SDL_GetTicks();
            uint32_t delta = now - lastTick;
            lastTick = now;

            padUpdate(&pad);
            u64 kDown = padGetButtonsDown(&pad);

            if (kDown & HidNpadButton_Plus)
                break;

            if (kDown & HidNpadButton_AnyUp) {
                if (cursor > 0) cursor--;
            }
            if (kDown & HidNpadButton_AnyDown) {
                if (cursor < (int)items.size() - 1) cursor++;
            }
            if (kDown & HidNpadButton_A) {
                if (cursor >= 0 && cursor < (int)items.size()) {
                    if (items[cursor].action == ListItem::SHOW_TOAST) {
                        toast.show("List Directory: sdmc:/",
                                   "No such device (open dir)");
                        printf("Toast triggered\n");
                    }
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

            // List
            for (int i = 0; i < (int)items.size(); i++) {
                int y = xplore::theme::HEADER_H + i * xplore::theme::ITEM_H;

                if (i == cursor) {
                    renderer.drawRectFilled(0, y,
                        xplore::theme::SCREEN_W, xplore::theme::ITEM_H,
                        xplore::theme::CURSOR_ROW);
                }

                fontManager.drawText(renderer.sdl(), items[i].label.c_str(),
                    xplore::theme::PADDING,
                    y + (xplore::theme::ITEM_H - xplore::theme::FONT_SIZE_ITEM) / 2,
                    xplore::theme::FONT_SIZE_ITEM, xplore::theme::TEXT);

                renderer.drawRectFilled(
                    xplore::theme::PADDING,
                    y + xplore::theme::ITEM_H - 1,
                    xplore::theme::SCREEN_W - xplore::theme::PADDING * 2, 1,
                    xplore::theme::DIVIDER);
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
