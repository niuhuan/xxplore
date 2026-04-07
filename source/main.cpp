#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <switch.h>

#include "ui/theme.hpp"
#include "ui/renderer.hpp"
#include "ui/font_manager.hpp"
#include "ui/file_list.hpp"
#include "ui/toast.hpp"
#include "fs/fs_api.hpp"

enum { ACTION_NONE = 0, ACTION_ENTER, ACTION_GO_UP };
enum ActivePanel { PANEL_LEFT, PANEL_RIGHT };

struct IconEntry {
    const char*  name;
    SDL_Texture* tex;
};

// --------------- animation ---------------

static float easeOutCubic(float t) {
    float t1 = 1.0f - t;
    return 1.0f - t1 * t1 * t1;
}

struct PanelAnim {
    float startLeftW  = 0.0f;
    float targetLeftW = 0.0f;
    float progress    = 1.0f;

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

// --------------- panel state ---------------

struct PanelState {
    std::string          path;
    xplore::FileList     list;
};

// --------------- icons ---------------

static std::vector<IconEntry> loadIcons(xplore::Renderer& renderer) {
    const char* names[] = {
        "folder", "file", "image", "video", "audio",
        "archive", "text", "code", "settings", "back"
    };
    std::vector<IconEntry> icons;
    for (auto n : names) {
        char p[128];
        snprintf(p, sizeof(p), "romfs:/icons/%s.png", n);
        SDL_Texture* tex = renderer.loadTexture(p);
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

// --------------- build list items from directory ---------------

/// Convert fs entries into ListItems with appropriate icons.
/// Prepends ".." (go-up) entry if we're not at the virtual root.
static std::vector<xplore::ListItem> buildItemsForPath(
    const std::string& path,
    const std::vector<IconEntry>& icons)
{
    std::vector<xplore::ListItem> items;

    if (xplore::fs::isVirtualRoot(path)) {
        auto roots = xplore::fs::getRootEntries();
        for (auto& r : roots) {
            items.push_back({r.name, findIcon(icons, "folder"), ACTION_ENTER});
        }
#ifdef XPLORE_DEBUG
        items.push_back({"Test Toast", nullptr, ACTION_NONE});
#endif
        return items;
    }

    // ".." entry to go up
    items.push_back({"..", findIcon(icons, "back"), ACTION_GO_UP});

    auto entries = xplore::fs::listDir(path);
    for (auto& e : entries) {
        int action = e.isDirectory ? ACTION_ENTER : ACTION_NONE;
        const char* iconName = xplore::fs::iconForEntry(e);
        items.push_back({e.name, findIcon(icons, iconName), action});
    }
    return items;
}

/// Navigate a panel to a new path. Reads directory and updates list.
static void navigatePanel(PanelState& panel,
                          const std::string& newPath,
                          const std::vector<IconEntry>& icons) {
    panel.path = newPath;
    panel.list.setItems(buildItemsForPath(newPath, icons));
    printf("Navigate: %s (%d items)\n", newPath.c_str(),
           static_cast<int>(panel.list.getItems().size()));
}

// --------------- main ---------------

int main(int argc, char* argv[]) {
#ifdef XPLORE_DEBUG
    socketInitializeDefault();
    nxlinkStdio();
    printf("Xplore: nxlink connected\n");
    printf("argv[0]: %s\n", (argc > 0 && argv && argv[0]) ? argv[0] : "");
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

        PanelState leftPanel;
        PanelState rightPanel;
        navigatePanel(leftPanel,  "/", icons);
        navigatePanel(rightPanel, "/", icons);

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

            // L/R panel switching
            if (!anim.isAnimating()) {
                if ((kDown & HidNpadButton_L) && activePanel != PANEL_LEFT) {
                    activePanel = PANEL_LEFT;
                    anim.start(anim.currentLeftW(),
                               static_cast<float>(ACTIVE_PANEL_W));
                }
                if ((kDown & HidNpadButton_R) && activePanel != PANEL_RIGHT) {
                    activePanel = PANEL_RIGHT;
                    anim.start(anim.currentLeftW(),
                               static_cast<float>(INACTIVE_PANEL_W));
                }
            }

            PanelState& active =
                (activePanel == PANEL_LEFT) ? leftPanel : rightPanel;
            xplore::FileList& activeList = active.list;

            if (kDown & HidNpadButton_AnyUp)
                activeList.moveCursorUp();
            if (kDown & HidNpadButton_AnyDown)
                activeList.moveCursorDown();

            if (kDown & HidNpadButton_A) {
                auto* item = activeList.getSelectedItem();
                if (item) {
                    if (item->action == ACTION_ENTER) {
                        std::string target;
                        if (xplore::fs::isVirtualRoot(active.path)) {
                            // Root entry like "sdmc:" → "sdmc:/"
                            target = item->label + "/";
                        } else {
                            target = xplore::fs::joinPath(active.path, item->label);
                        }
                        navigatePanel(active, target, icons);
                    } else if (item->action == ACTION_GO_UP) {
                        std::string parent = xplore::fs::parentPath(active.path);
                        navigatePanel(active, parent, icons);
                    }
                }
            }

            // B = go up in active panel
            if (kDown & HidNpadButton_B) {
                if (!xplore::fs::isVirtualRoot(active.path)) {
                    std::string parent = xplore::fs::parentPath(active.path);
                    navigatePanel(active, parent, icons);
                }
            }

            toast.update(delta);
            anim.update(delta);

            // --- Update off-screen caches ---
            int panelY = HEADER_H;
            int panelH = PANEL_CONTENT_H;
            leftPanel.list.updateCache(renderer, fontManager, ACTIVE_PANEL_W, panelH);
            rightPanel.list.updateCache(renderer, fontManager, ACTIVE_PANEL_W, panelH);

            // --- Composite to screen ---
            renderer.clear(xplore::theme::BG);

            // Header bar
            renderer.drawRectFilled(0, 0, SCREEN_W, HEADER_H, HEADER_BG);
            fontManager.drawText(renderer.sdl(), "Xplore",
                PADDING, (HEADER_H - FONT_SIZE_TITLE) / 2,
                FONT_SIZE_TITLE, PRIMARY);
            renderer.drawRectFilled(0, HEADER_H - 1, SCREEN_W, 1, DIVIDER);

            // Animated panel widths
            float leftWf = anim.currentLeftW();
            int   leftW  = static_cast<int>(leftWf);
            int   rightW = SCREEN_W - leftW;
            int   rightX = leftW;

            // Left panel
            renderer.setClipRect(0, panelY, leftW, panelH);
            renderer.drawTexture(leftPanel.list.getCachedTexture(),
                                 0, panelY, ACTIVE_PANEL_W, panelH);
            renderer.clearClipRect();
            if (activePanel != PANEL_LEFT)
                renderer.drawRectFilled(0, panelY, leftW, panelH, MASK_OVERLAY);

            // Divider
            renderer.drawRectFilled(rightX - 1, panelY, 2, panelH, DIVIDER);

            // Right panel
            renderer.setClipRect(rightX, panelY, rightW, panelH);
            renderer.drawTexture(rightPanel.list.getCachedTexture(),
                                 rightX, panelY, ACTIVE_PANEL_W, panelH);
            renderer.clearClipRect();
            if (activePanel != PANEL_RIGHT)
                renderer.drawRectFilled(rightX, panelY, rightW, panelH, MASK_OVERLAY);

            toast.render(renderer, fontManager);
            renderer.present();
        }

        leftPanel.list.destroyCache();
        rightPanel.list.destroyCache();
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
