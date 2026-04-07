#include <cstdio>
#include <string>
#include <vector>
#include <switch.h>

#include "ui/theme.hpp"
#include "ui/renderer.hpp"
#include "ui/font_manager.hpp"
#include "ui/file_list.hpp"
#include "ui/toast.hpp"

// Action ids for ListItem
enum { ACTION_NONE = 0, ACTION_SHOW_TOAST };
enum ActivePanel { PANEL_LEFT, PANEL_RIGHT };

/// Load a set of named icon textures from romfs:/icons/.
/// Returns a vector of {name, texture} pairs; caller must destroy textures.
struct IconEntry {
    const char* name;
    SDL_Texture* tex;
};

static std::vector<IconEntry> loadIcons(xplore::Renderer& renderer) {
    const char* names[] = {
        "folder", "file", "image", "video", "audio",
        "archive", "text", "code", "settings"
    };
    std::vector<IconEntry> icons;
    for (auto n : names) {
        char path[128];
        snprintf(path, sizeof(path), "romfs:/icons/%s.png", n);
        SDL_Texture* tex = renderer.loadTexture(path);
        icons.push_back({n, tex});
        printf("Icon %s: %s\n", n, tex ? "ok" : "FAILED");
    }
    return icons;
}

/// Find an icon texture by name from the loaded icon set.
static SDL_Texture* findIcon(const std::vector<IconEntry>& icons, const char* name) {
    for (auto& e : icons)
        if (strcmp(e.name, name) == 0) return e.tex;
    return nullptr;
}

/// Build the root-level list items (device list + debug entries).
static std::vector<xplore::ListItem> buildListItems(SDL_Texture* folderIcon) {
    std::vector<xplore::ListItem> items;
    items.push_back({"sdmc:", folderIcon, ACTION_NONE});
#ifdef XPLORE_DEBUG
    items.push_back({"Test Toast", nullptr, ACTION_SHOW_TOAST});
#endif
    return items;
}

int main(int argc, char* argv[]) {
#ifdef XPLORE_DEBUG
    socketInitializeDefault();
    nxlinkStdio();
    printf("Xplore: nxlink connected\n");
#endif

    romfsInit();

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    xplore::Renderer renderer;
    xplore::FontManager fontManager;
    bool ok = renderer.init();
    if (ok) ok = fontManager.init("romfs:/fonts/xplore.ttf");

    std::vector<IconEntry> icons;
    if (ok) icons = loadIcons(renderer);

    if (ok) {
        xplore::Toast toast;

        SDL_Texture* folderIcon = findIcon(icons, "folder");

        xplore::FileList leftList;
        xplore::FileList rightList;
        leftList.setItems(buildListItems(folderIcon));
        rightList.setItems(buildListItems(folderIcon));

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

            if (kDown & HidNpadButton_L) {
                activePanel = PANEL_LEFT;
                printf("Switch to LEFT panel\n");
            }
            if (kDown & HidNpadButton_R) {
                activePanel = PANEL_RIGHT;
                printf("Switch to RIGHT panel\n");
            }

            xplore::FileList& active =
                (activePanel == PANEL_LEFT) ? leftList : rightList;

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

            // Header bar
            renderer.drawRectFilled(0, 0,
                xplore::theme::SCREEN_W, xplore::theme::HEADER_H,
                xplore::theme::HEADER_BG);
            fontManager.drawText(renderer.sdl(), "Xplore",
                xplore::theme::PADDING,
                (xplore::theme::HEADER_H - xplore::theme::FONT_SIZE_TITLE) / 2,
                xplore::theme::FONT_SIZE_TITLE, xplore::theme::PRIMARY);
            renderer.drawRectFilled(0, xplore::theme::HEADER_H - 1,
                xplore::theme::SCREEN_W, 1, xplore::theme::DIVIDER);

            // Dual-panel geometry
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
                0, panelY, leftW, panelH, activePanel == PANEL_LEFT);
            if (activePanel != PANEL_LEFT)
                renderer.drawRectFilled(0, panelY, leftW, panelH,
                    xplore::theme::MASK_OVERLAY);

            // Divider between panels
            renderer.drawRectFilled(rightX - 1, panelY, 2, panelH,
                xplore::theme::DIVIDER);

            // Right panel
            rightList.render(renderer, fontManager,
                rightX, panelY, rightW, panelH, activePanel == PANEL_RIGHT);
            if (activePanel != PANEL_RIGHT)
                renderer.drawRectFilled(rightX, panelY, rightW, panelH,
                    xplore::theme::MASK_OVERLAY);

            toast.render(renderer, fontManager);
            renderer.present();
        }
    }

    // Cleanup icon textures
    for (auto& e : icons)
        if (e.tex) SDL_DestroyTexture(e.tex);

    fontManager.shutdown();
    renderer.shutdown();
    romfsExit();

#ifdef XPLORE_DEBUG
    socketExit();
#endif
    return 0;
}
