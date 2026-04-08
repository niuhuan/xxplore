#include "application.hpp"
#include "fs/fs_api.hpp"
#include "fs/clipboard.hpp"
#include "i18n/i18n.hpp"
#include "swkbd_input.hpp"
#include "ui/file_list.hpp"
#include "ui/font_manager.hpp"
#include "ui/image_viewer.hpp"
#include "ui/installer_screen.hpp"
#include "ui/main_menu.hpp"
#include "ui/modals.hpp"
#include "ui/renderer.hpp"
#include "ui/settings_screen.hpp"
#include "ui/theme.hpp"
#include "ui/toast.hpp"
#include "ui/websocket_installer_screen.hpp"
#include "util/app_config.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <switch.h>

namespace xplore {

namespace {

enum { ACTION_NONE = 0, ACTION_ENTER, ACTION_GO_UP, ACTION_WEBSOCKET_INSTALLER };
enum ActivePanel { PANEL_LEFT, PANEL_RIGHT };

struct IconEntry {
    const char*  name;
    SDL_Texture* tex;
};

struct PanelState {
    std::string      path;
    xplore::FileList list;
};

static float easeOutCubic(float t) {
    float u = 1.0f - t;
    return 1.0f - u * u * u;
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

constexpr uint64_t kAppletImageFileLimit   = 1ULL * 1024ULL * 1024ULL;
constexpr uint64_t kNormalImageFileLimit   = 10ULL * 1024ULL * 1024ULL;
constexpr uint64_t kAppletImageDecodeLimit = 8ULL * 1024ULL * 1024ULL;
constexpr uint64_t kNormalImageDecodeLimit = 96ULL * 1024ULL * 1024ULL;

static std::vector<IconEntry> loadIcons(Renderer& renderer) {
    const char* names[] = {
        "folder", "file", "image", "video", "audio",
        "archive", "text", "code", "settings", "download", "back"
    };
    std::vector<IconEntry> icons;
    for (auto n : names) {
        char p[128];
        snprintf(p, sizeof(p), "romfs:/icons/%s.png", n);
        SDL_Texture* tex = renderer.loadTexture(p);
        icons.push_back({n, tex});
    }
    return icons;
}

static SDL_Texture* findIcon(const std::vector<IconEntry>& icons, const char* name) {
    for (auto& e : icons)
        if (strcmp(e.name, name) == 0) return e.tex;
    return nullptr;
}

static std::vector<ListItem> buildItemsForPath(const std::string& path,
                                               const std::vector<IconEntry>& icons,
                                               const I18n& i18n) {
    std::vector<ListItem> items;

    if (fs::isVirtualRoot(path)) {
        auto roots = fs::getRootEntries();
        for (auto& r : roots)
            items.push_back({r.name, findIcon(icons, "folder"), ACTION_ENTER});
        items.push_back({i18n.t("root.websocket_installer"), findIcon(icons, "download"),
                         ACTION_WEBSOCKET_INSTALLER});
        return items;
    }

    items.push_back({"..", findIcon(icons, "back"), ACTION_GO_UP});
    auto entries = fs::listDir(path);
    for (auto& e : entries) {
        int action = e.isDirectory ? ACTION_ENTER : ACTION_NONE;
        items.push_back({e.name, findIcon(icons, fs::iconForEntry(e)), action});
    }
    return items;
}

static void navigatePanel(PanelState& panel, const std::string& newPath,
                          const std::vector<IconEntry>& icons, const I18n& i18n) {
    panel.path = newPath;
    bool hasGoUp = !fs::isVirtualRoot(newPath);
    bool allowSel = fs::pathAllowsSelection(newPath);
    panel.list.setItems(buildItemsForPath(newPath, icons, i18n), hasGoUp, allowSel);
}

static void reloadPanel(PanelState& panel, const std::vector<IconEntry>& icons,
                        const I18n& i18n, int preserveCursor) {
    bool hasGoUp = !fs::isVirtualRoot(panel.path);
    bool allowSel = fs::pathAllowsSelection(panel.path);
    panel.list.reloadItems(buildItemsForPath(panel.path, icons, i18n), hasGoUp, allowSel,
                           preserveCursor);
}

static void renderFooter(Renderer& renderer, FontManager& fontManager, const I18n& i18n) {
    using namespace theme;
    int footerY = HEADER_H + PANEL_CONTENT_H;
    renderer.drawRectFilled(0, footerY, SCREEN_W, FOOTER_H, HEADER_BG);
    renderer.drawRectFilled(0, footerY, SCREEN_W, 1, DIVIDER);

    struct Tip {
        const char* button;
        const char* key;
    };
    static const Tip tips[] = {
        {"A", "footer.a"},   {"B", "footer.b"}, {"Y", "footer.y"}, {"X", "footer.x"},
        {"L", "footer.l"},   {"R", "footer.r"}, {"+", "footer.plus"},
    };
    constexpr int n = 7;
    int spacing = 22;
    int totalW = 0;
    for (int i = 0; i < n; i++) {
        totalW += fontManager.measureText(tips[i].button, FONT_SIZE_FOOTER);
        totalW += fontManager.measureText(":", FONT_SIZE_FOOTER);
        totalW += fontManager.measureText(i18n.t(tips[i].key), FONT_SIZE_FOOTER);
    }
    int totalWithS = totalW + spacing * (n - 1);
    while (spacing > 10 && totalWithS > SCREEN_W - 24) {
        spacing -= 2;
        totalWithS = totalW + spacing * (n - 1);
    }
    int x = (SCREEN_W - totalWithS) / 2;
    int footerTextH = fontManager.fontHeight(FONT_SIZE_FOOTER);
    int y = footerY + (FOOTER_H - footerTextH) / 2 - 1;
    for (int i = 0; i < n; i++) {
        fontManager.drawText(renderer.sdl(), tips[i].button, x, y, FONT_SIZE_FOOTER, PRIMARY);
        x += fontManager.measureText(tips[i].button, FONT_SIZE_FOOTER);
        fontManager.drawText(renderer.sdl(), ":", x, y, FONT_SIZE_FOOTER, TEXT_SECONDARY);
        x += fontManager.measureText(":", FONT_SIZE_FOOTER);
        const char* act = i18n.t(tips[i].key);
        fontManager.drawText(renderer.sdl(), act, x, y, FONT_SIZE_FOOTER, TEXT_SECONDARY);
        x += fontManager.measureText(act, FONT_SIZE_FOOTER);
        x += spacing;
    }
}

enum class PendingConfirm { None, DeleteItems, PasteChoice };

} // namespace

int Application::run(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
#ifdef XPLORE_DEBUG
    socketInitializeDefault();
    nxlinkStdio();
#endif

    romfsInit();
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    const bool appletMode = appletGetAppletType() == AppletType_LibraryApplet;

    Renderer renderer;
    FontManager fontManager;
    if (!renderer.init() || !fontManager.init("romfs:/fonts/xplore.ttf")) {
        romfsExit();
        return 1;
    }

    config::AppConfig appConfig = config::defaultConfig();
    std::string configPath;
    config::loadConfigFromArgv0(argc > 0 ? argv[0] : nullptr, appConfig, configPath);

    I18n i18n;
    if (!i18n.load(config::languageRomfsPath(appConfig.language))) {
        appConfig = config::defaultConfig();
        i18n.load(config::languageRomfsPath(appConfig.language));
    }

    std::vector<IconEntry> icons = loadIcons(renderer);

    Toast toast;
    fs::Clipboard clipboard;
    BottomMainMenu mainMenu;
    ModalConfirm modalConfirm;
    ModalChoice  modalChoice;
    ModalProgress modalProgress;
    ModalInfo modalInfo;
    ModalInstallPrompt modalInstallPrompt;
    ImageViewer imageViewer;
    InstallerScreen installerScreen;
    SettingsScreen settingsScreen;
    WebSocketInstallerScreen webSocketInstallerScreen;

    PanelState leftPanel;
    PanelState rightPanel;
    navigatePanel(leftPanel, "/", icons, i18n);
    navigatePanel(rightPanel, "/", icons, i18n);

    ActivePanel activePanel = PANEL_LEFT;
    PanelAnim anim;
    anim.targetLeftW = static_cast<float>(theme::ACTIVE_PANEL_W);
    anim.startLeftW  = anim.targetLeftW;

    const int pageItems = theme::PANEL_CONTENT_H / theme::ITEM_H;
    uint32_t lastTick = SDL_GetTicks();

    PendingConfirm pendingConfirm = PendingConfirm::None;
    std::vector<std::string> pendingDeletePaths;
    std::string              savedDeleteDir;
    int                      savedDeleteCursor = 0;

    bool appQuit = false;
    std::vector<InstallQueueItem> pendingInstallItems;

    auto activeRef = [&]() -> PanelState& {
        return activePanel == PANEL_LEFT ? leftPanel : rightPanel;
    };

    auto refreshPath = [&](const std::string& dir, int keepCursor) {
        if (leftPanel.path == dir) {
            leftPanel.list.clearSelection();
            reloadPanel(leftPanel, icons, i18n, keepCursor);
        }
        if (rightPanel.path == dir) {
            rightPanel.list.clearSelection();
            reloadPanel(rightPanel, icons, i18n, keepCursor);
        }
    };

    auto refreshInstallAffectedDirs = [&](const std::vector<std::string>& dirs) {
        bool refreshLeft = false;
        bool refreshRight = false;
        for (const auto& dir : dirs) {
            if (leftPanel.path == dir)
                refreshLeft = true;
            if (rightPanel.path == dir)
                refreshRight = true;
        }

        if (refreshLeft) {
            int keepCursor = leftPanel.list.getCursor();
            leftPanel.list.clearSelection();
            reloadPanel(leftPanel, icons, i18n, keepCursor);
        }
        if (refreshRight) {
            int keepCursor = rightPanel.list.getCursor();
            rightPanel.list.clearSelection();
            reloadPanel(rightPanel, icons, i18n, keepCursor);
        }
    };

    auto appTitle = [&]() -> const char* {
        return appletMode ? "Xplore(Applet)" : "Xplore";
    };

    auto buildInstallItems = [&](PanelState& panel, bool useSelection) {
        std::vector<InstallQueueItem> result;
        const auto& items = panel.list.getItems();
        auto addIndex = [&](int index) {
            if (index < 0 || index >= static_cast<int>(items.size()))
                return;
            const ListItem& item = items[index];
            if (item.label == ".." || item.action == ACTION_ENTER)
                return;
            std::string fullPath = fs::joinPath(panel.path, item.label);
            if (!fs::isInstallPackagePath(fullPath))
                return;

            fs::FileStatInfo statInfo;
            if (!fs::statPath(fullPath, statInfo) || statInfo.isDirectory)
                return;

            InstallQueueItem queueItem;
            queueItem.path = fullPath;
            queueItem.name = item.label;
            queueItem.size = statInfo.size;
            result.push_back(std::move(queueItem));
        };

        if (useSelection && panel.list.hasSelection()) {
            for (int i = 0; i < static_cast<int>(items.size()); i++)
                if (panel.list.isSelected(i))
                    addIndex(i);
        } else {
            addIndex(panel.list.getCursor());
        }
        return result;
    };

    auto allSelectedInstallPackages = [&](PanelState& panel) -> bool {
        if (!panel.list.hasSelection())
            return false;

        const auto& items = panel.list.getItems();
        int selectedCount = 0;
        for (int i = 0; i < static_cast<int>(items.size()); i++) {
            if (!panel.list.isSelected(i))
                continue;
            selectedCount++;
            if (items[i].label == ".." || items[i].action == ACTION_ENTER)
                return false;
            std::string fullPath = fs::joinPath(panel.path, items[i].label);
            if (!fs::isInstallPackagePath(fullPath))
                return false;
        }
        return selectedCount > 0;
    };

    auto openInstallPrompt = [&](std::vector<InstallQueueItem> items) {
        if (items.empty())
            return;
        pendingInstallItems = std::move(items);

        std::string body;
        if (pendingInstallItems.size() == 1) {
            body = i18n.t("installer.prompt_single");
            body += " ";
            body += pendingInstallItems.front().name;
        } else {
            body = i18n.t("installer.prompt_multi_prefix");
            body += " ";
            body += std::to_string(pendingInstallItems.size());
            body += " ";
            body += i18n.t("installer.prompt_multi_suffix");
        }
        modalInstallPrompt.open(i18n.t("installer.prompt_title"), body);
    };

    auto openImageFile = [&](const std::string& path) {
        fs::FileStatInfo statInfo;
        if (!fs::statPath(path, statInfo) || statInfo.isDirectory) {
            toast.show(i18n.t("error.operation_failed"), "", ToastKind::Error, 2500);
            return;
        }

        const uint64_t fileLimit   = appletMode ? kAppletImageFileLimit : kNormalImageFileLimit;
        const uint64_t decodeLimit = appletMode ? kAppletImageDecodeLimit : kNormalImageDecodeLimit;
        if (statInfo.size > fileLimit) {
            toast.show(i18n.t("error.image_too_large"), "", ToastKind::Warning, 2500);
            return;
        }

        fs::ImageInfo imageInfo;
        std::string probeErr;
        if (!fs::probeImageInfo(path, imageInfo, probeErr)) {
            toast.show(i18n.t("error.image_load_failed"), probeErr.c_str(), ToastKind::Error, 3000);
            return;
        }

        uint64_t decodedBytes =
            static_cast<uint64_t>(imageInfo.width) * static_cast<uint64_t>(imageInfo.height) * 4ULL;
        if (decodedBytes > decodeLimit) {
            toast.show(i18n.t("error.image_too_large"), "", ToastKind::Warning, 2500);
            return;
        }

        std::string err;
        if (!imageViewer.open(renderer, path, err))
            toast.show(i18n.t("error.image_load_failed"), err.c_str(), ToastKind::Error, 3000);
    };

    auto openFilePath = [&](PanelState& panel, const ListItem& item) {
        if (item.label == "..")
            return;

        std::string fullPath = fs::joinPath(panel.path, item.label);
        if (fs::isInstallPackagePath(fullPath)) {
            openInstallPrompt(buildInstallItems(panel, false));
            return;
        }
        if (fs::isImagePath(fullPath)) {
            openImageFile(fullPath);
            return;
        }

        toast.show(i18n.t("error.open_not_supported"), item.label.c_str(), ToastKind::Info, 2500);
    };

    auto fillMenuState = [&](MainMenuState& st) {
        PanelState& a  = activeRef();
        bool vr        = fs::isVirtualRoot(a.path);
        bool allow     = fs::pathAllowsSelection(a.path);
        FileList& al   = a.list;

        if (vr || !allow) {
            st.disableSelectToggle = true;
            st.disableRename = st.disableNewFolder = st.disableDelete = true;
            st.disableCopy = st.disableCut = st.disablePaste = true;
            st.disableViewClip = st.disableClearClip = true;
            st.disableSettings = st.disableHelp = st.disableAbout = st.disableExit = false;
            char ctxBuf[256];
            snprintf(ctxBuf, sizeof(ctxBuf), "%s %s", i18n.t("menu.context_current"), i18n.t("menu.context_root"));
            st.contextLine = ctxBuf;
            return;
        }

        // Context line: "当前: path"
        {
            char ctxBuf[256];
            snprintf(ctxBuf, sizeof(ctxBuf), "%s %s", i18n.t("menu.context_current"), a.path.c_str());
            st.contextLine = ctxBuf;
        }

        int nSel = al.selectionCount();
        // const auto& items = al.getItems();
        const ListItem* cur = al.getSelectedItem();
        bool onGoUp         = cur && cur->label == "..";

        st.disableSelectToggle = onGoUp;
        st.disableRename =
            (nSel > 1) || onGoUp || (nSel == 0 && (!cur || onGoUp));
        st.disableNewFolder = false;

        bool canDelete = false;
        if (nSel > 0) canDelete = true;
        if (nSel == 0 && cur && !onGoUp) canDelete = true;
        st.disableDelete = !canDelete;

        bool hasTarget = (nSel > 0) || (cur && !onGoUp);
        st.disableCopy = st.disableCut = !hasTarget;

        bool clipEmpty = clipboard.empty();
        bool pasteDis  = clipEmpty;
        st.pasteFromCut = !clipEmpty && clipboard.operation() == fs::ClipboardOp::Cut;
        if (!clipEmpty && !fs::clipboardPasteDestinationAllowed(clipboard, a.path))
            pasteDis = true;
        st.disablePaste     = pasteDis;
        st.disableViewClip  = clipEmpty;
        st.disableClearClip = clipEmpty;
        st.disableSettings = st.disableHelp = st.disableAbout = st.disableExit = false;
    };

    auto renderScene = [&]() {
        int panelY_ = theme::HEADER_H;
        int panelH_ = theme::PANEL_CONTENT_H;
        float lwf   = anim.currentLeftW();
        int lw      = static_cast<int>(lwf);
        int rw      = theme::SCREEN_W - lw;
        int rx      = lw;
        renderer.clear(theme::BG);
        renderer.drawRectFilled(0, 0, theme::SCREEN_W, theme::HEADER_H, theme::HEADER_BG);
        fontManager.drawText(renderer.sdl(), appTitle(), theme::PADDING,
                             (theme::HEADER_H - theme::FONT_SIZE_TITLE) / 2,
                             theme::FONT_SIZE_TITLE, theme::PRIMARY);
        renderer.drawRectFilled(0, theme::HEADER_H - 1, theme::SCREEN_W, 1, theme::DIVIDER);
        renderer.setClipRect(0, panelY_, lw, panelH_);
        renderer.drawTexture(leftPanel.list.getCachedTexture(), 0, panelY_,
                             theme::ACTIVE_PANEL_W, panelH_);
        renderer.clearClipRect();
        if (activePanel != PANEL_LEFT)
            renderer.drawRectFilled(0, panelY_, lw, panelH_, theme::MASK_OVERLAY);
        renderer.drawRectFilled(rx - 1, panelY_, 2, panelH_, theme::DIVIDER);
        renderer.setClipRect(rx, panelY_, rw, panelH_);
        renderer.drawTexture(rightPanel.list.getCachedTexture(), rx, panelY_,
                             theme::ACTIVE_PANEL_W, panelH_);
        renderer.clearClipRect();
        if (activePanel != PANEL_RIGHT)
            renderer.drawRectFilled(rx, panelY_, rw, panelH_, theme::MASK_OVERLAY);
    };

    bool interrupted = false;

    auto pumpProgress = [&]() -> bool {
        padUpdate(&pad);
        u64 k = padGetButtonsDown(&pad);
        if (k & HidNpadButton_Minus) interrupted = true;
        renderScene();
        int scrimH = theme::HEADER_H + theme::PANEL_CONTENT_H;
        renderer.drawRectFilled(0, 0, theme::SCREEN_W, scrimH, theme::MENU_SCRIM_CONTENT);
        renderFooter(renderer, fontManager, i18n);
        modalProgress.render(renderer, fontManager, i18n);
        renderer.present();
        return !interrupted;
    };

    while (appletMainLoop()) {
        uint32_t now   = SDL_GetTicks();
        uint32_t delta = now - lastTick;
        lastTick       = now;

        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        PanelState& active   = activeRef();
        FileList& activeList = active.list;

        bool modalBlocking =
            modalConfirm.isOpen() || modalChoice.isOpen() || modalProgress.isOpen() ||
            modalInfo.isOpen() || modalInstallPrompt.isOpen() || imageViewer.isOpen() ||
            installerScreen.isOpen() || settingsScreen.isOpen() || webSocketInstallerScreen.isOpen();
        bool menuBlocking = mainMenu.isOpen();

        // Plus toggles menu
        if (kDown & HidNpadButton_Plus) {
            if (installerScreen.isOpen() || imageViewer.isOpen() || webSocketInstallerScreen.isOpen()) {
                // Reserved for screen-specific handling below.
            } else if (mainMenu.isOpen())
                mainMenu.close();
            else if (!modalBlocking)
                mainMenu.open();
        }

        if (imageViewer.isOpen()) {
            imageViewer.handleInput(kDown);
        }

        if (settingsScreen.isOpen()) {
            SettingsAction action = settingsScreen.handleInput(kDown);
            if (action == SettingsAction::Close) {
                settingsScreen.close();
            } else if (action == SettingsAction::Save) {
                config::AppConfig nextConfig = appConfig;
                nextConfig.language = settingsScreen.selectedLanguage();
                std::string err;
                if (!config::saveConfig(configPath, nextConfig, err)) {
                    toast.show(i18n.t("error.operation_failed"), err.c_str(), ToastKind::Error, 3200);
                } else {
                    appConfig = nextConfig;
                    if (!i18n.load(config::languageRomfsPath(appConfig.language))) {
                        toast.show(i18n.t("error.operation_failed"), "load language failed",
                                   ToastKind::Error, 3200);
                    } else {
                        reloadPanel(leftPanel, icons, i18n, leftPanel.list.getCursor());
                        reloadPanel(rightPanel, icons, i18n, rightPanel.list.getCursor());
                        settingsScreen.close();
                    }
                }
            }
        }

        if (webSocketInstallerScreen.isOpen()) {
            WebSocketInstallerAction action = webSocketInstallerScreen.handleInput(kDown);
            if (action == WebSocketInstallerAction::Close)
                webSocketInstallerScreen.close();
        }

        if (installerScreen.isOpen()) {
            installerScreen.update();
            InstallerAction action = installerScreen.handleInput(kDown);
            if (action == InstallerAction::Close) {
                if (installerScreen.shouldRefreshOnClose())
                    refreshInstallAffectedDirs(installerScreen.sourceDirectories());
                installerScreen.close();
                pendingInstallItems.clear();
            }
        }

        if (modalConfirm.isOpen()) {
            ConfirmResult cr = modalConfirm.handleInput(kDown);
            if (cr == ConfirmResult::Confirmed) {
                if (pendingConfirm == PendingConfirm::DeleteItems) {
                    modalConfirm.close();
                    pendingConfirm = PendingConfirm::None;
                    interrupted = false;
                    modalProgress.open(i18n.t("progress.deleting"), "");
                    bool delErr = false;
                    std::string delLastErr;
                    for (const auto& p : pendingDeletePaths) {
                        modalProgress.setDetail(p);
                        if (!pumpProgress()) break;
                        std::string err;
                        if (!fs::removeAll(p, err)) { delErr = true; delLastErr = err; }
                    }
                    modalProgress.close();
                    refreshPath(savedDeleteDir, savedDeleteCursor);
                    pendingDeletePaths.clear();
                    if (interrupted) {
                        toast.show(i18n.t("toast.interrupted"), "", ToastKind::Warning, 4000);
                        clipboard.clear();
                    } else if (delErr)
                        toast.show(i18n.t("error.operation_failed"), delLastErr.c_str(), ToastKind::Error, 3000);
                    else
                        toast.show(i18n.t("toast.deleted"), "", ToastKind::Success, 2200);
                }
            } else if (cr == ConfirmResult::Cancelled) {
                modalConfirm.close();
                pendingConfirm = PendingConfirm::None;
                pendingDeletePaths.clear();
            }
        }

        if (modalChoice.isOpen()) {
            ChoiceResult ch = modalChoice.handleInput(kDown);
            if (ch != ChoiceResult::None) {
                modalChoice.close();
                if (ch == ChoiceResult::Cancel || pendingConfirm != PendingConfirm::PasteChoice) {
                    pendingConfirm = PendingConfirm::None;
                } else {
                    pendingConfirm = PendingConfirm::None;
                    bool isCut = (clipboard.operation() == fs::ClipboardOp::Cut);
                    std::string destDir = active.path;
                    int savedCursor = activeList.getCursor();
                    interrupted = false;
                    const char* progressTitle = isCut ? i18n.t("progress.moving") : i18n.t("progress.copying");
                    modalProgress.open(progressTitle, "");
                    bool anyError = false;
                    std::string lastErr;
                    auto onProgress = [&](const std::string& f) -> bool {
                        modalProgress.setDetail(f);
                        return pumpProgress();
                    };
                    for (const auto& e : clipboard.items()) {
                        if (interrupted) break;
                        std::string src = e.fullPath;
                        std::string dst = fs::joinPath(destDir, e.name);
                        std::string err;
                        bool ok = true;
                        if (ch == ChoiceResult::Merge) {
                            ok = isCut ? fs::moveEntryMerge(src, dst, err, onProgress)
                                       : fs::copyEntryMerge(src, dst, err, onProgress);
                        } else if (ch == ChoiceResult::Overwrite) {
                            ok = isCut ? fs::moveEntryOverwrite(src, dst, err, onProgress)
                                       : fs::copyEntryOverwrite(src, dst, err, onProgress);
                        }
                        if (!ok && err != "interrupted") { anyError = true; lastErr = err; }
                    }
                    modalProgress.close();
                    clipboard.clear();
                    activeList.clearSelection();
                    refreshPath(destDir, savedCursor);
                    if (interrupted)
                        toast.show(i18n.t("toast.interrupted"), "", ToastKind::Warning, 4000);
                    else if (anyError)
                        toast.show(i18n.t("error.operation_failed"), lastErr.c_str(), ToastKind::Error, 3000);
                    else
                        toast.show(i18n.t(isCut ? "toast.moved" : "toast.copied"), "", ToastKind::Success, 2200);
                }
            }
        }

        if (modalInfo.isOpen()) {
            if (modalInfo.handleInput(kDown) != ConfirmResult::None) modalInfo.close();
        }

        if (modalInstallPrompt.isOpen()) {
            InstallPromptResult result = modalInstallPrompt.handleInput(kDown);
            if (result != InstallPromptResult::None) {
                modalInstallPrompt.close();
                if (result == InstallPromptResult::Install ||
                    result == InstallPromptResult::InstallAndDelete) {
                    installerScreen.open(
                        pendingInstallItems,
                        result == InstallPromptResult::InstallAndDelete
                            ? InstallDeleteMode::DeleteAfterInstall
                            : InstallDeleteMode::KeepFiles,
                        appletMode);
                    activeList.clearSelection();
                } else {
                    pendingInstallItems.clear();
                }
            }
        }

        if (!modalBlocking && !menuBlocking) {
            if (!anim.isAnimating()) {
                if ((kDown & HidNpadButton_L) && activePanel != PANEL_LEFT) {
                    activePanel = PANEL_LEFT;
                    anim.start(anim.currentLeftW(), static_cast<float>(theme::ACTIVE_PANEL_W));
                }
                if ((kDown & HidNpadButton_R) && activePanel != PANEL_RIGHT) {
                    activePanel = PANEL_RIGHT;
                    anim.start(anim.currentLeftW(), static_cast<float>(theme::INACTIVE_PANEL_W));
                }
            }

            if (kDown & HidNpadButton_AnyUp) activeList.moveCursorUp();
            if (kDown & HidNpadButton_AnyDown) activeList.moveCursorDown();
            if (kDown & HidNpadButton_AnyLeft) activeList.moveCursorPageUp(pageItems);
            if (kDown & HidNpadButton_AnyRight) activeList.moveCursorPageDown(pageItems);

            if (kDown & HidNpadButton_A) {
                auto* item = activeList.getSelectedItem();
                if (item) {
                    if (item->action == ACTION_ENTER) {
                        bool currentSelected =
                            activeList.hasSelection() && activeList.isSelected(activeList.getCursor());
                        if (activeList.hasSelection() && currentSelected) {
                            if (allSelectedInstallPackages(active))
                                openInstallPrompt(buildInstallItems(active, true));
                            else
                                toast.show(i18n.t("error.multi_open_not_supported"), "",
                                           ToastKind::Info, 2600);
                        } else {
                            std::string target;
                            if (fs::isVirtualRoot(active.path))
                                target = item->label + "/";
                            else
                                target = fs::joinPath(active.path, item->label);
                            navigatePanel(active, target, icons, i18n);
                        }
                    } else if (item->action == ACTION_GO_UP) {
                        navigatePanel(active, fs::parentPath(active.path), icons, i18n);
                    } else if (item->action == ACTION_WEBSOCKET_INSTALLER) {
                        activeList.clearSelection();
                        webSocketInstallerScreen.open(i18n);
                    } else {
                        bool hasSelection    = activeList.hasSelection();
                        bool currentSelected = hasSelection && activeList.isSelected(activeList.getCursor());
                        if (hasSelection && currentSelected) {
                            if (allSelectedInstallPackages(active))
                                openInstallPrompt(buildInstallItems(active, true));
                            else
                                toast.show(i18n.t("error.multi_open_not_supported"), "",
                                           ToastKind::Info, 2600);
                        } else {
                            if (hasSelection)
                                activeList.clearSelection();
                            openFilePath(active, *item);
                        }
                    }
                }
            }

            if (kDown & HidNpadButton_B) {
                if (!fs::isVirtualRoot(active.path))
                    navigatePanel(active, fs::parentPath(active.path), icons, i18n);
            }

            if (kDown & HidNpadButton_Y) activeList.toggleSelect();
            if (kDown & HidNpadButton_X) activeList.toggleSelectAll();
        }

        MainMenuState menuSt;
        fillMenuState(menuSt);

        if (mainMenu.isOpen()) {
            mainMenu.update(delta, kDown, menuSt);
            MenuCommand cmd = mainMenu.takeCommand();
            switch (cmd) {
            case MenuCommand::CloseMenu: mainMenu.close(); break;
            case MenuCommand::ExitApp:
                mainMenu.close();
                appQuit = true;
                break;
            case MenuCommand::ToggleSelectMode:
                activeList.toggleSelect();
                mainMenu.close();
                break;
            case MenuCommand::Settings:
                settingsScreen.open(appConfig.language);
                mainMenu.close();
                break;
            case MenuCommand::Help:
                modalInfo.open(i18n.t("menu.help"), i18n.t("help.short"));
                mainMenu.close();
                break;
            case MenuCommand::About:
                modalInfo.open(i18n.t("menu.about"), i18n.t("help.about_body"));
                mainMenu.close();
                break;
            case MenuCommand::ViewClipboard: {
                char header[512];
                const char* hdrFmt = clipboard.operation() == fs::ClipboardOp::Cut
                                         ? i18n.t("clipboard.header_cut")
                                         : i18n.t("clipboard.header_copy");
                snprintf(header, sizeof(header), hdrFmt, clipboard.sourceDirectory().c_str());
                std::string body = header;
                body += '\n';
                for (const auto& e : clipboard.items()) {
                    body += e.isDirectory ? i18n.t("clipboard.prefix_folder")
                                          : i18n.t("clipboard.prefix_file");
                    body += e.name;
                    body += '\n';
                }
                if (clipboard.items().empty()) body += i18n.t("clipboard.empty");
                modalInfo.open(i18n.t("menu.clipboard_view"), body);
                mainMenu.close();
                break;
            }
            case MenuCommand::ClearClipboard:
                clipboard.clear();
                toast.show(i18n.t("toast.clipboard_cleared"), "", ToastKind::Info, 2000);
                mainMenu.close();
                break;
            case MenuCommand::Copy:
            case MenuCommand::Cut: {
                std::vector<fs::ClipboardEntry> ents;
                const auto& items = activeList.getItems();
                auto addItem = [&](int i) {
                    if (i < 0 || i >= (int)items.size()) return;
                    if (items[i].label == "..") return;
                    fs::ClipboardEntry ce;
                    ce.name        = items[i].label;
                    ce.isDirectory = (items[i].action == ACTION_ENTER);
                    ce.fullPath    = fs::joinPath(active.path, items[i].label);
                    ents.push_back(std::move(ce));
                };
                if (activeList.hasSelection()) {
                    for (int i = 0; i < (int)items.size(); i++)
                        if (activeList.isSelected(i)) addItem(i);
                } else {
                    const ListItem* it = activeList.getSelectedItem();
                    if (it && it->label != "..") {
                        for (int i = 0; i < (int)items.size(); i++) {
                            if (items[i].label == it->label) {
                                addItem(i);
                                break;
                            }
                        }
                    }
                }
                if (ents.empty()) break;
                clipboard.set(active.path, std::move(ents),
                                cmd == MenuCommand::Cut ? fs::ClipboardOp::Cut : fs::ClipboardOp::Copy);
                activeList.clearSelection();
                toast.show(i18n.t(cmd == MenuCommand::Cut ? "toast.cut" : "toast.copied"), "", ToastKind::Success, 2000);
                mainMenu.close();
                break;
            }
            case MenuCommand::Paste: {
                if (clipboard.empty()) break;
                if (!fs::clipboardPasteDestinationAllowed(clipboard, active.path)) {
                    toast.show(i18n.t("error.operation_failed"), i18n.t("error.paste_forbidden"), ToastKind::Warning, 3200);
                    break;
                }
                mainMenu.close();
                // Check if any destination already exists
                bool anyExists = false;
                for (const auto& e : clipboard.items()) {
                    if (fs::pathExists(fs::joinPath(active.path, e.name))) { anyExists = true; break; }
                }
                if (anyExists) {
                    bool isCut = (clipboard.operation() == fs::ClipboardOp::Cut);
                    const char* t = isCut ? i18n.t("confirm.move_conflict_title") : i18n.t("confirm.copy_conflict_title");
                    const char* b = isCut ? i18n.t("confirm.move_conflict_body") : i18n.t("confirm.copy_conflict_body");
                    modalChoice.open(t, b);
                    pendingConfirm = PendingConfirm::PasteChoice;
                } else {
                    bool isCut = (clipboard.operation() == fs::ClipboardOp::Cut);
                    std::string destDir = active.path;
                    int savedCursor = activeList.getCursor();
                    interrupted = false;
                    const char* progressTitle = isCut ? i18n.t("progress.moving") : i18n.t("progress.copying");
                    modalProgress.open(progressTitle, "");
                    bool anyError = false;
                    std::string lastErr;
                    auto onProgress = [&](const std::string& f) -> bool {
                        modalProgress.setDetail(f);
                        return pumpProgress();
                    };
                    for (const auto& e : clipboard.items()) {
                        if (interrupted) break;
                        std::string src = e.fullPath;
                        std::string dst = fs::joinPath(destDir, e.name);
                        std::string err;
                        bool ok = isCut ? fs::moveEntrySimple(src, dst, err, onProgress)
                                        : fs::copyEntrySimple(src, dst, err, onProgress);
                        if (!ok && err != "interrupted") { anyError = true; lastErr = err; }
                    }
                    modalProgress.close();
                    clipboard.clear();
                    activeList.clearSelection();
                    refreshPath(destDir, savedCursor);
                    if (interrupted)
                        toast.show(i18n.t("toast.interrupted"), "", ToastKind::Warning, 4000);
                    else if (anyError)
                        toast.show(i18n.t("error.operation_failed"), lastErr.c_str(), ToastKind::Error, 3000);
                    else
                        toast.show(i18n.t(isCut ? "toast.moved" : "toast.copied"), "", ToastKind::Success, 2200);
                }
                break;
            }
            case MenuCommand::Delete: {
                pendingDeletePaths.clear();
                const auto& items = activeList.getItems();
                if (activeList.hasSelection()) {
                    for (int i = 0; i < (int)items.size(); i++) {
                        if (!activeList.isSelected(i)) continue;
                        if (items[i].label == "..") continue;
                        pendingDeletePaths.push_back(fs::joinPath(active.path, items[i].label));
                    }
                } else {
                    const ListItem* it = activeList.getSelectedItem();
                    if (it && it->label != "..")
                        pendingDeletePaths.push_back(fs::joinPath(active.path, it->label));
                }
                if (pendingDeletePaths.empty()) break;
                savedDeleteDir    = active.path;
                savedDeleteCursor = activeList.getCursor();
                modalConfirm.open(i18n.t("confirm.delete_title"), i18n.t("confirm.delete_body"));
                pendingConfirm = PendingConfirm::DeleteItems;
                mainMenu.close();
                break;
            }
            case MenuCommand::NewFolder: {
                char buf[256] = {0};
                if (!swkbdTextInput(i18n.t("swkbd.new_folder_title"), i18n.t("swkbd.new_folder_guide"),
                                    "NewFolder", buf, sizeof(buf)))
                    break;
                std::string name(buf);
                if (!fs::isValidEnglishFileName(name)) {
                    toast.show(i18n.t("error.invalid_name"), "", ToastKind::Warning, 3000);
                    break;
                }
                std::string full = fs::joinPath(active.path, name);
                if (fs::pathExists(full)) {
                    toast.show(i18n.t("error.exists"), "", ToastKind::Warning, 3000);
                    break;
                }
                std::string err;
                if (!fs::createDirectory(full, err)) {
                    toast.show(i18n.t("error.operation_failed"), err.c_str(), ToastKind::Error, 3000);
                    break;
                }
                int kc = activeList.getCursor();
                refreshPath(active.path, kc);
                mainMenu.close();
                break;
            }
            case MenuCommand::Rename: {
                const ListItem* target = nullptr;
                const auto& items      = activeList.getItems();
                if (activeList.selectionCount() == 1) {
                    for (int i = 0; i < (int)items.size(); i++) {
                        if (activeList.isSelected(i)) {
                            target = &items[i];
                            break;
                        }
                    }
                } else {
                    const ListItem* cur = activeList.getSelectedItem();
                    if (cur && cur->label != "..") {
                        for (int i = 0; i < (int)items.size(); i++) {
                            if (&items[i] == cur) {
                                target = &items[i];
                                break;
                            }
                        }
                    }
                }
                if (!target) break;
                char buf[256] = {0};
                if (!swkbdTextInput(i18n.t("swkbd.rename_title"), i18n.t("swkbd.rename_guide"),
                                    target->label.c_str(), buf, sizeof(buf)))
                    break;
                std::string newName(buf);
                if (!fs::isValidEnglishFileName(newName)) {
                    toast.show(i18n.t("error.invalid_name"), "", ToastKind::Warning, 3000);
                    break;
                }
                std::string from = fs::joinPath(active.path, target->label);
                std::string to   = fs::joinPath(active.path, newName);
                if (from == to) break;
                if (fs::pathExists(to)) {
                    toast.show(i18n.t("error.exists"), "", ToastKind::Warning, 3000);
                    break;
                }
                std::string err;
                if (!fs::renamePath(from, to, err)) {
                    toast.show(i18n.t("error.operation_failed"), err.c_str(), ToastKind::Error, 3000);
                    break;
                }
                int kc = activeList.getCursor();
                refreshPath(active.path, kc);
                mainMenu.close();
                break;
            }
            default: break;
            }
        }

        toast.update(delta);
        anim.update(delta);

        leftPanel.list.updateCache(renderer, fontManager, theme::ACTIVE_PANEL_W,
                                   theme::PANEL_CONTENT_H);
        rightPanel.list.updateCache(renderer, fontManager, theme::ACTIVE_PANEL_W,
                                    theme::PANEL_CONTENT_H);

        renderScene();

        const bool anyOverlay =
            mainMenu.isOpen() || modalConfirm.isOpen() || modalChoice.isOpen()
            || modalProgress.isOpen() || modalInfo.isOpen() || modalInstallPrompt.isOpen();
        if (anyOverlay && !imageViewer.isOpen() && !installerScreen.isOpen()
            && !settingsScreen.isOpen() && !webSocketInstallerScreen.isOpen()) {
            int scrimH = theme::HEADER_H + theme::PANEL_CONTENT_H;
            renderer.drawRectFilled(0, 0, theme::SCREEN_W, scrimH, theme::MENU_SCRIM_CONTENT);
        }
        if (!imageViewer.isOpen() && !installerScreen.isOpen()
            && !settingsScreen.isOpen() && !webSocketInstallerScreen.isOpen())
            renderFooter(renderer, fontManager, i18n);

        if (mainMenu.isOpen()) mainMenu.render(renderer, fontManager, i18n, menuSt);
        modalConfirm.render(renderer, fontManager, i18n);
        modalChoice.render(renderer, fontManager, i18n);
        modalProgress.render(renderer, fontManager, i18n);
        modalInfo.render(renderer, fontManager);
        modalInstallPrompt.render(renderer, fontManager, i18n);
        imageViewer.render(renderer);
        installerScreen.render(renderer, fontManager, i18n);
        settingsScreen.render(renderer, fontManager, i18n);
        webSocketInstallerScreen.render(renderer, fontManager, i18n);

        toast.render(renderer, fontManager);
        renderer.present();

        if (appQuit) break;
    }

    leftPanel.list.destroyCache();
    rightPanel.list.destroyCache();
    webSocketInstallerScreen.close();
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

} // namespace xplore
