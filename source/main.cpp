#include <cstdio>
#include <cmath>
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

struct IconEntry {
    const char*  name;
    SDL_Texture* tex;
};

static float easeOutCubic(float t) {
    float t1 = 1.0f - t;
    return 1.0f - t1 * t1 * t1;
}

struct PanelAnim {
    float startLeftW  = 0.0f;
    float targetLeftW = 0.0f;
    float progress    = 1.0f;   // 1 = idle / finished

    void start(float from, float to) {
        startLeftW  = from;
        targetLeftW = to;
        progress    = 0.0f;
    }

    void update(uint32_t deltaMs) {
        if (progress >= 1.0f) return;
        progress += static_cast<float>(deltaMs) / xplore::theme::ANIM_DURATION_F;
        if (progress > 1.0f) progress = 1.0f;
    }

    float currentLeftW() const {
        if (progress >= 1.0f) return targetLeftW;
        float t = easeOutCubic(progress);
        return startLeftW + (targetLeftW - startLeftW) * t;
    }

    bool isAnimating() const { return progress < 1.0f; }
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

static SDL_Texture* findIcon(const std::vector<IconEntry>& icons, const char* name) {
    for (auto& e : icons)
        if (strcmp(e.name, name) == 0) return e.tex;
    return nullptr;
}

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
        using namespace xplore::theme;

        xplore::Toast toast;

        SDL_Texture* folderIcon = findIcon(icons, "folder");

        xplore::FileList leftList;
        xplore::FileList rightList;
        leftList.setItems(buildListItems(folderIcon));
        rightList.setItems(buildListItems(folderIcon));

        ActivePanel activePanel = PANEL_LEFT;

        PanelAnim anim;
        anim.targetLeftW = static_cast<float>(ACTIVE_PANEL_W);
        anim.startLeftW  = anim.targetLeftW;

        uint32_t lastTick = SDL_GetTicks();

        while (appletMainLoop()) {
            uint32_t now   = SDL_GetTicks();
            uint32_t delta = now - lastTick;
            lastTick = now;

            padUpdate(&pad);
            u64 kDown = padGetButtonsDown(&pad);

            if (kDown & HidNpadButton_Plus)
                break;

            // L/R panel switching — blocked while animating
            if (!anim.isAnimating()) {
                if ((kDown & HidNpadButton_L) && activePanel != PANEL_LEFT) {
                    activePanel = PANEL_LEFT;
                    anim.start(anim.currentLeftW(),
                               static_cast<float>(ACTIVE_PANEL_W));
                    printf("Switch to LEFT panel\n");
                }
                if ((kDown & HidNpadButton_R) && activePanel != PANEL_RIGHT) {
                    activePanel = PANEL_RIGHT;
                    anim.start(anim.currentLeftW(),
                               static_cast<float>(INACTIVE_PANEL_W));
                    printf("Switch to RIGHT panel\n");
                }
            }

            // Up/Down/A always go to the active panel
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
            anim.update(delta);

            // --- Update off-screen caches (only redraws when dirty) ---
            int panelY = HEADER_H;
            int panelH = PANEL_CONTENT_H;
            leftList.updateCache(renderer, fontManager, ACTIVE_PANEL_W, panelH);
            rightList.updateCache(renderer, fontManager, ACTIVE_PANEL_W, panelH);

            // --- Composite to screen ---
            renderer.clear(xplore::theme::BG);

            // Header bar
            renderer.drawRectFilled(0, 0, SCREEN_W, HEADER_H, HEADER_BG);
            fontManager.drawText(renderer.sdl(), "Xplore",
                PADDING, (HEADER_H - FONT_SIZE_TITLE) / 2,
                FONT_SIZE_TITLE, PRIMARY);
            renderer.drawRectFilled(0, HEADER_H - 1, SCREEN_W, 1, DIVIDER);

            // Animated panel widths
            float leftWf  = anim.currentLeftW();
            int   leftW   = static_cast<int>(leftWf);
            int   rightW  = SCREEN_W - leftW;
            int   rightX  = leftW;

            // Left panel — clip to current animated width, draw cached texture
            renderer.setClipRect(0, panelY, leftW, panelH);
            renderer.drawTexture(leftList.getCachedTexture(),
                                 0, panelY, ACTIVE_PANEL_W, panelH);
            renderer.clearClipRect();
            if (activePanel != PANEL_LEFT)
                renderer.drawRectFilled(0, panelY, leftW, panelH, MASK_OVERLAY);

            // Divider line between panels
            renderer.drawRectFilled(rightX - 1, panelY, 2, panelH, DIVIDER);

            // Right panel — clip to current animated width, draw cached texture
            renderer.setClipRect(rightX, panelY, rightW, panelH);
            renderer.drawTexture(rightList.getCachedTexture(),
                                 rightX, panelY, ACTIVE_PANEL_W, panelH);
            renderer.clearClipRect();
            if (activePanel != PANEL_RIGHT)
                renderer.drawRectFilled(rightX, panelY, rightW, panelH, MASK_OVERLAY);

            toast.render(renderer, fontManager);
            renderer.present();
        }

        leftList.destroyCache();
        rightList.destroyCache();
    }

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
