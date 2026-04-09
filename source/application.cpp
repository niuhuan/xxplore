#include "application.hpp"
#include "fs/fs_api.hpp"
#include "fs/clipboard.hpp"
#include "fs/provider_manager.hpp"
#include "fs/smb_provider.hpp"
#include "fs/usb_drive_manager.hpp"
#include "fs/usb_mount_provider.hpp"
#include "fs/webdav_provider.hpp"
#include "i18n/i18n.hpp"
#include "swkbd_input.hpp"
#include "ui/file_list.hpp"
#include "ui/font_manager.hpp"
#include "ui/image_viewer.hpp"
#include "ui/installer_screen.hpp"
#include "ui/loading_overlay.hpp"
#include "ui/main_menu.hpp"
#include "ui/modals.hpp"
#include "ui/network_drive_form.hpp"
#include "ui/renderer.hpp"
#include "ui/settings_screen.hpp"
#include "ui/theme.hpp"
#include "ui/touch_event.hpp"
#include "ui/toast.hpp"
#include "ui/websocket_installer_screen.hpp"
#include "util/app_config.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <switch.h>

namespace xxplore {

namespace {

enum { ACTION_NONE = 0, ACTION_ENTER, ACTION_GO_UP, ACTION_WEBSOCKET_INSTALLER,
       ACTION_ADD_NETWORK_DRIVE };
enum ActivePanel { PANEL_LEFT, PANEL_RIGHT };

enum class InputLayer {
    BasePanels,
    MainMenu,
    ModalConfirm,
    ModalChoice,
    ModalProgress,
    ModalInfo,
    ModalInstallPrompt,
    ImageViewer,
    InstallerScreen,
    SettingsScreen,
    WebSocketInstallerScreen,
    NetworkDriveForm,
    LoadingOverlay,
};

struct IconEntry {
    const char*  name;
    SDL_Texture* tex;
};

struct HeaderIconLayout {
    int aboutHitX = 0;
    int menuHitX = 0;
    int hitY = 0;
    int hitW = 0;
    int hitH = 0;
    int aboutIconX = 0;
    int menuIconX = 0;
    int iconY = 0;
    int iconSize = 0;
};

struct FooterTouchButtonsLayout {
    bool enabled = false;
    int prevX = 0;
    int nextX = 0;
    int pageW = 0;
    int selectAllX = 0;
    int selectAllW = 0;
    int selectX = 0;
    int selectW = 0;
    int y = 0;
    int h = 0;
    int contentLeft = 0;
    int contentRight = 0;
};

struct PanelState {
    std::string      path;
    xxplore::FileList list;
};

struct PendingPanelLoad {
    bool                  active = false;
    ActivePanel           panel = PANEL_LEFT;
    std::string           path;
    std::vector<ListItem> items;
    std::string           err;
    bool                  finished = false;
    std::mutex            mutex;
    std::thread           worker;

    void join() {
        if (worker.joinable())
            worker.join();
    }
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
        progress += static_cast<float>(deltaMs) / xxplore::theme::ANIM_DURATION_F;
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
constexpr std::size_t kAppletFontGlyphCacheLimit = 2U * 1024U * 1024U;
constexpr std::size_t kNormalFontGlyphCacheLimit = 4U * 1024U * 1024U;

static std::string deriveSiblingFontPath(const char* argv0) {
    if (!argv0 || !argv0[0])
        return {};

    std::string path(argv0);
    std::size_t slash = path.find_last_of("/\\");
    std::size_t dot = path.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return {};

    path.replace(dot, std::string::npos, ".ttf");
    return path;
}

static bool fileExists(const std::string& path) {
    if (path.empty())
        return false;

    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;

    std::fclose(file);
    return true;
}

static bool readTextFile(const std::string& path, std::string& out) {
    out.clear();
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;

    char buffer[1024];
    std::size_t readBytes = 0;
    while ((readBytes = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        out.append(buffer, readBytes);

    std::fclose(file);
    return !out.empty();
}

static bool startsWith(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

static bool isNetworkProvider(const fs::FileProvider* provider) {
    if (!provider)
        return false;
    return provider->kind() == fs::ProviderKind::WebDav ||
           provider->kind() == fs::ProviderKind::Smb;
}

static bool isUsbProvider(const fs::FileProvider* provider) {
    return provider && provider->kind() == fs::ProviderKind::Usb;
}

static std::vector<IconEntry> loadIcons(Renderer& renderer) {
    const char* names[] = {
        "folder", "file", "image", "video", "audio",
        "archive", "text", "code", "settings", "download", "back",
        "network", "add", "sdcard", "usb", "about", "menu"
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

static HeaderIconLayout headerIconLayout() {
    HeaderIconLayout layout;
    layout.iconSize = 24;
    layout.hitW = 42;
    layout.hitH = 36;
    layout.hitY = (theme::HEADER_H - layout.hitH) / 2;
    layout.iconY = (theme::HEADER_H - layout.iconSize) / 2;
    constexpr int kEdgePadding = 8;
    constexpr int kGap = 10;
    layout.menuHitX = theme::SCREEN_W - kEdgePadding - layout.hitW;
    layout.aboutHitX = layout.menuHitX - kGap - layout.hitW;
    layout.menuIconX = layout.menuHitX + (layout.hitW - layout.iconSize) / 2;
    layout.aboutIconX = layout.aboutHitX + (layout.hitW - layout.iconSize) / 2;
    return layout;
}

static FooterTouchButtonsLayout footerTouchButtonsLayout(bool enabled) {
    FooterTouchButtonsLayout layout;
    if (!enabled) {
        layout.contentLeft = theme::PADDING;
        layout.contentRight = theme::SCREEN_W - theme::PADDING;
        return layout;
    }

    constexpr int kButtonGap = 12;
    constexpr int kSidePadding = 14;
    layout.enabled = true;
    layout.pageW = 42;
    layout.selectW = 42;
    layout.selectAllW = 42;
    layout.h = 28;
    layout.y = theme::HEADER_H + theme::PANEL_CONTENT_H + (theme::FOOTER_H - layout.h) / 2;
    layout.prevX = kSidePadding;
    layout.nextX = layout.prevX + layout.pageW + kButtonGap;
    layout.selectX = theme::SCREEN_W - kSidePadding - layout.selectW;
    layout.selectAllX = layout.selectX - kButtonGap - layout.selectAllW;
    layout.contentLeft = layout.nextX + layout.pageW + 20;
    layout.contentRight = layout.selectAllX - 20;
    return layout;
}

static void drawThickChevron(Renderer& renderer, int x, int y, int w, int h, bool left,
                             SDL_Color color) {
    int cx = x + w / 2;
    int cy = y + h / 2;
    int halfH = 7;
    int halfW = 4;
    int dir = left ? -1 : 1;
    for (int offset = -1; offset <= 1; ++offset) {
        renderer.drawLine(cx + dir * halfW, cy - halfH + offset, cx - dir * halfW, cy + offset,
                          color);
        renderer.drawLine(cx - dir * halfW, cy + offset, cx + dir * halfW, cy + halfH + offset,
                          color);
    }
}

static void drawSelectIcon(Renderer& renderer, int x, int y, int w, int h, SDL_Color color,
                           bool multiple) {
    int frontW = 11;
    int frontH = 11;
    int frontX = x + (w - frontW) / 2 + (multiple ? 2 : 0);
    int frontY = y + (h - frontH) / 2 + (multiple ? 1 : 0);
    if (multiple)
        renderer.drawRect(frontX - 4, frontY - 3, frontW, frontH, color);
    renderer.drawRect(frontX, frontY, frontW, frontH, color);
    renderer.drawLine(frontX + 2, frontY + 6, frontX + 5, frontY + 9, color);
    renderer.drawLine(frontX + 5, frontY + 9, frontX + 10, frontY + 3, color);
    renderer.drawLine(frontX + 2, frontY + 7, frontX + 5, frontY + 10, color);
    renderer.drawLine(frontX + 5, frontY + 10, frontX + 10, frontY + 4, color);
}

static bool parseSmbAddress(const std::string& address, std::string& server,
                            std::string& share) {
    server = address;
    share.clear();
    auto slash = server.find('/');
    if (slash == std::string::npos)
        return false;
    share = server.substr(slash + 1);
    server = server.substr(0, slash);
    return !server.empty() && !share.empty();
}

static bool buildItemsForPath(const std::string& path, const std::vector<IconEntry>& icons,
                              const I18n& i18n, fs::ProviderManager& provMgr,
                              std::vector<ListItem>& itemsOut, std::string* errOut = nullptr) {
    std::vector<ListItem> items;

    if (fs::ProviderManager::isVirtualRoot(path)) {
        auto roots = provMgr.getRootEntries();
        for (auto& r : roots) {
            const char* iconName = "folder";
            auto* prov = provMgr.findProviderByDisplayPrefix(r.name);
            if (prov) {
                if (prov->kind() == fs::ProviderKind::Local) {
                    iconName = "sdcard";
                } else if (isNetworkProvider(prov)) {
                    iconName = "network";
                } else if (isUsbProvider(prov)) {
                    iconName = "usb";
                }
            } else if (r.name == "sdmc:") {
                iconName = "sdcard";
            } else if (startsWith(r.name, "ums")) {
                iconName = "usb";
            } else if (r.name.find("(WebDAV):") != std::string::npos ||
                       r.name.find("(SMB):") != std::string::npos) {
                iconName = "network";
            }
            ListItem li;
            li.label = r.name;
            li.icon = findIcon(icons, iconName);
            li.action = ACTION_ENTER;
            li.size = r.size;
            if (prov)
                li.metadata = prov->providerId();
            items.push_back(std::move(li));
        }
        items.push_back({i18n.t("root.websocket_installer"), findIcon(icons, "download"),
                         ACTION_WEBSOCKET_INSTALLER, 0});
        items.push_back({i18n.t("root.add_network_drive"), findIcon(icons, "add"),
                         ACTION_ADD_NETWORK_DRIVE, 0});
        itemsOut = std::move(items);
        return true;
    }

    items.push_back({"..", findIcon(icons, "back"), ACTION_GO_UP, 0});
    std::string errListDir;
    auto entries = provMgr.listDir(path, errListDir);
    if (!errListDir.empty()) {
        if (errOut)
            *errOut = errListDir;
        return false;
    }
    for (auto& e : entries) {
        int action = e.isDirectory ? ACTION_ENTER : ACTION_NONE;
        items.push_back({e.name, findIcon(icons, fs::iconForEntry(e)), action, e.size});
    }
    itemsOut = std::move(items);
    return true;
}

static bool navigatePanel(PanelState& panel, const std::string& newPath,
                          const std::vector<IconEntry>& icons, const I18n& i18n,
                          fs::ProviderManager& provMgr, std::string* errOut = nullptr) {
    std::vector<ListItem> items;
    if (!buildItemsForPath(newPath, icons, i18n, provMgr, items, errOut))
        return false;

    bool hasGoUp = !fs::ProviderManager::isVirtualRoot(newPath);
    bool allowSel = provMgr.pathAllowsSelection(newPath);
    panel.path = newPath;
    panel.list.setItems(std::move(items), hasGoUp, allowSel);
    return true;
}

static bool reloadPanel(PanelState& panel, const std::vector<IconEntry>& icons,
                        const I18n& i18n, int preserveCursor,
                        fs::ProviderManager& provMgr, std::string* errOut = nullptr) {
    std::vector<ListItem> items;
    if (!buildItemsForPath(panel.path, icons, i18n, provMgr, items, errOut))
        return false;

    bool hasGoUp = !fs::ProviderManager::isVirtualRoot(panel.path);
    bool allowSel = provMgr.pathAllowsSelection(panel.path);
    panel.list.reloadItems(std::move(items), hasGoUp, allowSel, preserveCursor);
    return true;
}

static void renderFooter(Renderer& renderer, FontManager& fontManager, const I18n& i18n,
                         bool touchButtonsEnabled) {
    using namespace theme;
    int footerY = HEADER_H + PANEL_CONTENT_H;
    renderer.drawRectFilled(0, footerY, SCREEN_W, FOOTER_H, HEADER_BG);
    renderer.drawRectFilled(0, footerY, SCREEN_W, 1, DIVIDER);

    const FooterTouchButtonsLayout touchLayout = footerTouchButtonsLayout(touchButtonsEnabled);
    if (touchLayout.enabled) {
        struct FooterButton {
            int x;
            int w;
        };
        const FooterButton buttons[] = {
            {touchLayout.prevX, touchLayout.pageW},
            {touchLayout.nextX, touchLayout.pageW},
            {touchLayout.selectAllX, touchLayout.selectAllW},
            {touchLayout.selectX, touchLayout.selectW},
        };
        for (const auto& button : buttons) {
            renderer.drawRoundedRectFilled(button.x, touchLayout.y, button.w, touchLayout.h, 8,
                                           SURFACE);
            renderer.drawRoundedRect(button.x, touchLayout.y, button.w, touchLayout.h, 8,
                                     DIVIDER);
        }
        drawThickChevron(renderer, touchLayout.prevX, touchLayout.y, touchLayout.pageW,
                         touchLayout.h, true, PRIMARY);
        drawThickChevron(renderer, touchLayout.nextX, touchLayout.y, touchLayout.pageW,
                         touchLayout.h, false, PRIMARY);
        drawSelectIcon(renderer, touchLayout.selectAllX, touchLayout.y, touchLayout.selectAllW,
                       touchLayout.h, PRIMARY, true);
        drawSelectIcon(renderer, touchLayout.selectX, touchLayout.y, touchLayout.selectW,
                       touchLayout.h, PRIMARY, false);
    }

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
    int contentLeft = touchLayout.enabled ? touchLayout.contentLeft : PADDING;
    int contentRight = touchLayout.enabled ? touchLayout.contentRight : (SCREEN_W - PADDING);
    int availableW = contentRight - contentLeft;
    int totalWithS = totalW + spacing * (n - 1);
    while (spacing > 10 && totalWithS > availableW) {
        spacing -= 2;
        totalWithS = totalW + spacing * (n - 1);
    }
    int x = contentLeft + std::max(0, (availableW - totalWithS) / 2);
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

enum class PendingConfirm { None, DeleteItems, PasteChoice, DeleteDrive, UnmountDrive };

} // namespace

int Application::run(int argc, char* argv[]) {
    (void)argc;
#ifdef XPLORE_DEBUG
    socketInitializeDefault();
    nxlinkStdio();
#endif

    romfsInit();
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    const bool appletMode = appletGetAppletType() == AppletType_LibraryApplet;
    const std::size_t fontGlyphCacheLimit =
        appletMode ? kAppletFontGlyphCacheLimit : kNormalFontGlyphCacheLimit;
    const char* argv0 = (argv && argc > 0) ? argv[0] : nullptr;
    const std::string externalFontPath = deriveSiblingFontPath(argv0);
    const std::string selectedFontPath =
        fileExists(externalFontPath) ? externalFontPath : "romfs:/fonts/xxplore.ttf";

#ifdef XPLORE_DEBUG
    std::printf("Font: path=%s cache_limit=%zuKB\n",
                selectedFontPath.c_str(), fontGlyphCacheLimit / 1024U);
#endif

    Renderer renderer;
    FontManager fontManager;
    if (!renderer.init() || !fontManager.init(selectedFontPath.c_str(), fontGlyphCacheLimit)) {
        romfsExit();
        return 1;
    }

    config::AppConfig appConfig = config::defaultConfig();
    std::string configPath;
    config::loadConfigFromArgv0(argv0, appConfig, configPath);

    I18n i18n;
    if (!i18n.load(config::languageRomfsPath(appConfig.language))) {
        appConfig = config::defaultConfig();
        i18n.load(config::languageRomfsPath(appConfig.language));
    }

    std::vector<IconEntry> icons = loadIcons(renderer);

    // Provider manager: register local + network drives from config
    fs::ProviderManager provMgr;
    for (const auto& drive : appConfig.networkDrives) {
        std::shared_ptr<fs::FileProvider> prov;
        if (drive.type == config::NetworkDriveType::WebDAV) {
            prov = std::make_shared<fs::WebDavProvider>(
                drive.id, drive.name, drive.address, drive.username, drive.password);
        }
        else if (drive.type == config::NetworkDriveType::SMB2) {
            std::string server;
            std::string share;
            if (parseSmbAddress(drive.address, server, share)) {
                prov = std::make_shared<fs::SmbProvider>(
                    drive.id, drive.name, server, share, drive.username, drive.password);
            }
        }
        if (prov) provMgr.registerProvider(prov);
    }
    fs::UsbDriveManager usbDriveManager;
    std::vector<fs::UsbDriveInfo> usbDrives;
    {
        std::string usbErr;
        if (!usbDriveManager.init(usbErr)) {
#ifdef XPLORE_DEBUG
            std::printf("USB: init skipped (%s)\n", usbErr.c_str());
#endif
        } else {
            usbDrives = usbDriveManager.snapshot();
            for (const auto& drive : usbDrives) {
                provMgr.registerProvider(std::make_shared<fs::UsbMountProvider>(
                    drive.providerId, drive.mountName, drive.readOnly));
            }
        }
    }

    Toast toast;
    fs::Clipboard clipboard;
    BottomMainMenu mainMenu;
    ModalConfirm modalConfirm;
    ModalChoice  modalChoice;
    ModalProgress modalProgress;
    ModalErrorAction modalErrorAction;
    ModalInfo modalInfo;
    ModalInstallPrompt modalInstallPrompt;
    ImageViewer imageViewer;
    InstallerScreen installerScreen;
    SettingsScreen settingsScreen;
    WebSocketInstallerScreen webSocketInstallerScreen;
    NetworkDriveForm networkDriveForm;
    LoadingOverlay loadingOverlay;

    PanelState leftPanel;
    PanelState rightPanel;
    navigatePanel(leftPanel, "/", icons, i18n, provMgr);
    navigatePanel(rightPanel, "/", icons, i18n, provMgr);

    ActivePanel activePanel = PANEL_LEFT;
    PanelAnim anim;
    anim.targetLeftW = static_cast<float>(theme::ACTIVE_PANEL_W);
    anim.startLeftW  = anim.targetLeftW;

    const int pageItems = theme::PANEL_CONTENT_H / theme::ITEM_H;
    uint32_t lastTick = SDL_GetTicks();
    bool touchWasDown = false;

    PendingConfirm pendingConfirm = PendingConfirm::None;
    std::vector<std::string> pendingDeletePaths;
    std::string              savedDeleteDir;
    int                      savedDeleteCursor = 0;

    bool appQuit = false;
    std::vector<InstallQueueItem> pendingInstallItems;
    PendingPanelLoad pendingPanelLoad;

    auto activeRef = [&]() -> PanelState& {
        return activePanel == PANEL_LEFT ? leftPanel : rightPanel;
    };

    auto panelFor = [&](ActivePanel panel) -> PanelState& {
        return panel == PANEL_LEFT ? leftPanel : rightPanel;
    };

    auto parseSmbAddress = [](const std::string& address, std::string& server,
                              std::string& share) -> bool {
        server = address;
        share.clear();
        auto slash = server.find('/');
        if (slash == std::string::npos)
            return false;
        share = server.substr(slash + 1);
        server = server.substr(0, slash);
        return !server.empty() && !share.empty();
    };

    auto providerForRootItem = [&](const ListItem* item) -> fs::FileProvider* {
        if (!item)
            return nullptr;
        if (!item->metadata.empty()) {
            auto* prov = provMgr.findProvider(item->metadata);
            if (prov)
                return prov;
        }
        return provMgr.findProviderByDisplayPrefix(item->label);
    };

    auto applyUsbSnapshot = [&](const std::vector<fs::UsbDriveInfo>& drives,
                                bool showRemovalToast) {
        std::vector<std::string> removedPrefixes;
        for (const auto& existing : usbDrives) {
            bool stillPresent = false;
            for (const auto& next : drives) {
                if (next.providerId == existing.providerId) {
                    stillPresent = true;
                    break;
                }
            }
            if (!stillPresent) {
                removedPrefixes.push_back(existing.mountName);
                provMgr.removeProvider(existing.providerId);
            }
        }

        usbDrives = drives;
        for (const auto& drive : usbDrives) {
            provMgr.registerProvider(std::make_shared<fs::UsbMountProvider>(
                drive.providerId, drive.mountName, drive.readOnly));
        }

        auto panelUsesRemovedUsb = [&](const PanelState& panel) {
            if (fs::ProviderManager::isVirtualRoot(panel.path))
                return false;
            const std::string prefix = fs::ProviderManager::extractPrefix(panel.path);
            for (const auto& removed : removedPrefixes)
                if (prefix == removed)
                    return true;
            return false;
        };

        bool rootChanged = fs::ProviderManager::isVirtualRoot(leftPanel.path) ||
                           fs::ProviderManager::isVirtualRoot(rightPanel.path);
        if (rootChanged) {
            if (fs::ProviderManager::isVirtualRoot(leftPanel.path))
                reloadPanel(leftPanel, icons, i18n, leftPanel.list.getCursor(), provMgr);
            if (fs::ProviderManager::isVirtualRoot(rightPanel.path))
                reloadPanel(rightPanel, icons, i18n, rightPanel.list.getCursor(), provMgr);
        }

        bool leftRemoved = panelUsesRemovedUsb(leftPanel);
        bool rightRemoved = panelUsesRemovedUsb(rightPanel);
        if (leftRemoved)
            navigatePanel(leftPanel, "/", icons, i18n, provMgr);
        if (rightRemoved)
            navigatePanel(rightPanel, "/", icons, i18n, provMgr);

        if (showRemovalToast && (!removedPrefixes.empty()) && !modalConfirm.isOpen()) {
            toast.show(i18n.t("toast.usb_removed"), "", ToastKind::Info, 2200);
        }
    };

    auto refreshPath = [&](const std::string& dir, int keepCursor) {
        if (leftPanel.path == dir) {
            leftPanel.list.clearSelection();
            reloadPanel(leftPanel, icons, i18n, keepCursor, provMgr);
        }
        if (rightPanel.path == dir) {
            rightPanel.list.clearSelection();
            reloadPanel(rightPanel, icons, i18n, keepCursor, provMgr);
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
            reloadPanel(leftPanel, icons, i18n, keepCursor, provMgr);
        }
        if (refreshRight) {
            int keepCursor = rightPanel.list.getCursor();
            rightPanel.list.clearSelection();
            reloadPanel(rightPanel, icons, i18n, keepCursor, provMgr);
        }
    };

    auto appTitle = [&]() -> const char* {
        return appletMode ? "Xxplore(Applet)" : "Xxplore";
    };

    auto localizedDocText = [&](const char* docName, const char* fallbackKey) {
        std::string path = "romfs:/i18n/";
        path += config::languageId(appConfig.language);
        path += ".";
        path += docName;
        path += ".txt";

        std::string content;
        if (readTextFile(path, content))
            return content;
        return std::string(i18n.t(fallbackKey));
    };

    auto startPathLoad = [&](ActivePanel panel, const std::string& targetPath) {
        if (pendingPanelLoad.active)
            return;

        pendingPanelLoad.join();
        pendingPanelLoad.active = true;
        pendingPanelLoad.panel = panel;
        pendingPanelLoad.path = targetPath;
        pendingPanelLoad.items.clear();
        pendingPanelLoad.err.clear();
        pendingPanelLoad.finished = false;

        loadingOverlay.show(i18n.t("loading.connecting"), 15000, 180);
#ifdef XPLORE_DEBUG
        std::printf("[nav] async load start panel=%s path=%s\n",
                    panel == PANEL_LEFT ? "left" : "right", targetPath.c_str());
#endif
        pendingPanelLoad.worker = std::thread([&icons, &i18n, &provMgr, &pendingPanelLoad,
                                               targetPath]() {
            std::vector<ListItem> items;
            std::string err;
            buildItemsForPath(targetPath, icons, i18n, provMgr, items, &err);

            std::lock_guard<std::mutex> lock(pendingPanelLoad.mutex);
            pendingPanelLoad.items = std::move(items);
            pendingPanelLoad.err = std::move(err);
            pendingPanelLoad.finished = true;
        });
    };

    auto navigateToPath = [&](ActivePanel panel, const std::string& targetPath) {
        PanelState& targetPanel = panelFor(panel);
        if (!provMgr.isNetworkPath(targetPath)) {
            std::string err;
            if (!navigatePanel(targetPanel, targetPath, icons, i18n, provMgr, &err))
                toast.show(i18n.t("error.operation_failed"), err.c_str(), ToastKind::Error, 3200);
            return;
        }

        startPathLoad(panel, targetPath);
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

            InstallQueueItem queueItem;
            queueItem.path = fullPath;
            queueItem.name = item.label;
            queueItem.size = item.size;
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

    auto startInstaller = [&](std::vector<InstallQueueItem> items) {
        if (items.empty())
            return;
        auto sourceCallbacks = std::make_shared<InstallDataSourceCallbacks>();
        sourceCallbacks->readRange =
            [&provMgr](const InstallQueueItem& item, uint64_t offset, size_t size,
                       void* outBuffer, std::string& errOut) {
                return provMgr.readFile(item.path, offset, size, outBuffer, errOut);
            };
        installerScreen.open(std::move(items), InstallDeleteMode::KeepFiles, appletMode,
                             sourceCallbacks);
        activeRef().list.clearSelection();
        pendingInstallItems.clear();
    };

    auto installItemsForMenu = [&](PanelState& panel) {
        bool hasSelection = panel.list.hasSelection();
        bool currentSelected = hasSelection && panel.list.isSelected(panel.list.getCursor());
        if (hasSelection && allSelectedInstallPackages(panel))
            return buildInstallItems(panel, true);
        if (!hasSelection || !currentSelected)
            return buildInstallItems(panel, false);
        return std::vector<InstallQueueItem>{};
    };

    auto openImageFile = [&](const std::string& path) {
        fs::FileStatInfo statInfo;
        std::string statErr;
        if (!provMgr.statPath(path, statInfo, statErr) || statInfo.isDirectory) {
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
        bool vr        = fs::ProviderManager::isVirtualRoot(a.path);
        bool allow     = provMgr.pathAllowsSelection(a.path);
        FileList& al   = a.list;

        if (vr) {
            // In virtual root: most ops disabled, but some drive-level actions remain.
            st.disableSelectToggle = true;
            st.disableRename = true;
            st.disableNewFolder = true;
            st.disableDelete = true;
            st.disableCopy = st.disableCut = st.disablePaste = true;
            st.disableViewClip = st.disableClearClip = st.disableInstall = true;
            st.disableSettings = st.disableHelp = st.disableAbout = st.disableExit = false;

            const ListItem* cur = al.getSelectedItem();
            auto* driveProv = providerForRootItem(cur);
            if (driveProv) {
                if (isNetworkProvider(driveProv)) {
                    st.renameIsEdit = true;
                    st.deleteIsDriveDel = true;
                    st.disableRename = false;
                    st.disableDelete = false;
                } else if (isUsbProvider(driveProv)) {
                    st.deleteIsUnmount = true;
                    st.disableDelete = false;
                }
            }

            char ctxBuf[256];
            snprintf(ctxBuf, sizeof(ctxBuf), "%s %s", i18n.t("menu.context_current"), i18n.t("menu.context_root"));
            st.contextLine = ctxBuf;
            return;
        }

        if (!allow) {
            st.disableSelectToggle = true;
            st.disableRename = st.disableNewFolder = st.disableDelete = true;
            st.disableCopy = st.disableCut = st.disablePaste = true;
            st.disableViewClip = st.disableClearClip = st.disableInstall = true;
            st.disableSettings = st.disableHelp = st.disableAbout = st.disableExit = false;
            char ctxBuf[256];
            snprintf(ctxBuf, sizeof(ctxBuf), "%s %s", i18n.t("menu.context_current"), a.path.c_str());
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
        st.disableInstall   = installItemsForMenu(a).empty();
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
        HeaderIconLayout headerIcons = headerIconLayout();
        const char* titleText = appTitle();
        int titleX = theme::PADDING;
        int titleTextH = fontManager.fontHeight(theme::FONT_SIZE_TITLE);
        int titleY = (theme::HEADER_H - titleTextH) / 2;
        fontManager.drawText(renderer.sdl(), titleText, titleX, titleY,
                             theme::FONT_SIZE_TITLE, theme::PRIMARY);
        if (!fs::ProviderManager::isVirtualRoot(activeRef().path)) {
            constexpr int kTitleGap = 18;
            constexpr int kIconGap = 18;
            int pathX = titleX + fontManager.measureText(titleText, theme::FONT_SIZE_TITLE) +
                        kTitleGap;
            int pathMaxW = headerIcons.aboutHitX - kIconGap - pathX;
            if (pathMaxW > 24) {
                int pathTextH = fontManager.fontHeight(theme::FONT_SIZE_SMALL);
                int pathY = (theme::HEADER_H - pathTextH) / 2;
                fontManager.drawTextEllipsis(renderer.sdl(), activeRef().path.c_str(), pathX,
                                             pathY, theme::FONT_SIZE_SMALL,
                                             theme::TEXT_DISABLED, pathMaxW);
            }
        }
        if (SDL_Texture* aboutIcon = findIcon(icons, "about")) {
            renderer.drawTexture(aboutIcon, headerIcons.aboutIconX, headerIcons.iconY,
                                 headerIcons.iconSize, headerIcons.iconSize);
        }
        if (SDL_Texture* menuIcon = findIcon(icons, "menu")) {
            renderer.drawTexture(menuIcon, headerIcons.menuIconX, headerIcons.iconY,
                                 headerIcons.iconSize, headerIcons.iconSize);
        }
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
        renderFooter(renderer, fontManager, i18n, appConfig.touchButtonsEnabled);
        modalProgress.render(renderer, fontManager, i18n);
        renderer.present();
        return !interrupted;
    };

    auto promptTransferError = [&](const fs::TransferError& error) -> fs::TransferDecision {
        const char* titleKey = "error.operation_failed";
        switch (error.operation) {
        case fs::TransferOperation::Copy: titleKey = "error.copy_failed"; break;
        case fs::TransferOperation::Move: titleKey = "error.move_failed"; break;
        case fs::TransferOperation::Delete: titleKey = "error.delete_failed"; break;
        }

        std::string body = error.currentPath;
        if (!error.targetPath.empty()) {
            body += " -> ";
            body += error.targetPath;
        }

        modalErrorAction.open(i18n.t(titleKey), body, error.message);
        for (;;) {
            padUpdate(&pad);
            u64 k = padGetButtonsDown(&pad);
            HidTouchScreenState touchState {};
            hidGetTouchScreenStates(&touchState, 1);
            TouchTap tap;
            if (touchState.count > 0 && !touchWasDown) {
                tap.active = true;
                tap.x = static_cast<int>(touchState.touches[0].x);
                tap.y = static_cast<int>(touchState.touches[0].y);
            }
            touchWasDown = touchState.count > 0;

            ErrorActionResult action = modalErrorAction.handleInput(k, &tap);
            renderScene();
            int scrimH = theme::HEADER_H + theme::PANEL_CONTENT_H;
            renderer.drawRectFilled(0, 0, theme::SCREEN_W, scrimH, theme::MENU_SCRIM_CONTENT);
            renderFooter(renderer, fontManager, i18n, appConfig.touchButtonsEnabled);
            modalProgress.render(renderer, fontManager, i18n);
            modalErrorAction.render(renderer, fontManager, i18n);
            renderer.present();

            switch (action) {
            case ErrorActionResult::Abort:
                modalErrorAction.close();
                return fs::TransferDecision::Abort;
            case ErrorActionResult::Ignore:
                modalErrorAction.close();
                return fs::TransferDecision::Ignore;
            case ErrorActionResult::IgnoreAll:
                modalErrorAction.close();
                return fs::TransferDecision::IgnoreAll;
            case ErrorActionResult::None:
            default:
                break;
            }
            SDL_Delay(16);
        }
    };

    auto makeTransferCallbacks = [&](fs::TransferOperation op) -> fs::TransferCallbacks {
        fs::TransferCallbacks callbacks;
        callbacks.onProgress = [&](const fs::TransferProgress& progress) -> bool {
            (void)op;
            modalProgress.setDetail(progress.currentPath);
            modalProgress.setProgress(progress.overallBytes, progress.overallTotalBytes);
            return pumpProgress();
        };
        callbacks.onError = [&](const fs::TransferError& error) -> fs::TransferDecision {
            return promptTransferError(error);
        };
        return callbacks;
    };

    while (appletMainLoop()) {
        uint32_t now   = SDL_GetTicks();
        uint32_t delta = now - lastTick;
        lastTick       = now;

        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);
        HidTouchScreenState touchState {};
        hidGetTouchScreenStates(&touchState, 1);
        TouchTap tap;
        if (touchState.count > 0 && !touchWasDown) {
            tap.active = true;
            tap.x = static_cast<int>(touchState.touches[0].x);
            tap.y = static_cast<int>(touchState.touches[0].y);
        }
        touchWasDown = touchState.count > 0;

        std::vector<fs::UsbDriveInfo> changedUsbDrives;
        if (usbDriveManager.consumeChanged(changedUsbDrives))
            applyUsbSnapshot(changedUsbDrives, true);

        PanelState& active   = activeRef();
        FileList& activeList = active.list;
        // Touch and gamepad share one dispatcher: only the top-most layer gets this frame's input.
        InputLayer inputLayer = InputLayer::BasePanels;
        if (pendingPanelLoad.active || loadingOverlay.isActive())
            inputLayer = InputLayer::LoadingOverlay;
        else if (networkDriveForm.isOpen())
            inputLayer = InputLayer::NetworkDriveForm;
        else if (webSocketInstallerScreen.isOpen())
            inputLayer = InputLayer::WebSocketInstallerScreen;
        else if (settingsScreen.isOpen())
            inputLayer = InputLayer::SettingsScreen;
        else if (installerScreen.isOpen())
            inputLayer = InputLayer::InstallerScreen;
        else if (imageViewer.isOpen())
            inputLayer = InputLayer::ImageViewer;
        else if (modalInstallPrompt.isOpen())
            inputLayer = InputLayer::ModalInstallPrompt;
        else if (modalInfo.isOpen())
            inputLayer = InputLayer::ModalInfo;
        else if (modalProgress.isOpen())
            inputLayer = InputLayer::ModalProgress;
        else if (modalChoice.isOpen())
            inputLayer = InputLayer::ModalChoice;
        else if (modalConfirm.isOpen())
            inputLayer = InputLayer::ModalConfirm;
        else if (mainMenu.isOpen())
            inputLayer = InputLayer::MainMenu;

        bool updateMainMenu = false;
        MainMenuState menuInputState;
        if (inputLayer == InputLayer::MainMenu)
            fillMenuState(menuInputState);

        switch (inputLayer) {
        case InputLayer::LoadingOverlay:
        case InputLayer::ModalProgress:
            break;

        case InputLayer::ImageViewer:
            imageViewer.handleInput(kDown);
            break;

        case InputLayer::SettingsScreen: {
            SettingsAction action = settingsScreen.handleInput(kDown, &tap);
            if (action == SettingsAction::Close) {
                settingsScreen.close();
            } else if (action == SettingsAction::Save) {
                config::AppConfig nextConfig = appConfig;
                nextConfig.language = settingsScreen.selectedLanguage();
                nextConfig.touchButtonsEnabled = settingsScreen.touchButtonsEnabled();
                std::string err;
                if (!config::saveConfig(configPath, nextConfig, err)) {
                    toast.show(i18n.t("error.operation_failed"), err.c_str(), ToastKind::Error, 3200);
                } else {
                    appConfig = nextConfig;
                    if (!i18n.load(config::languageRomfsPath(appConfig.language))) {
                        toast.show(i18n.t("error.operation_failed"), "load language failed",
                                   ToastKind::Error, 3200);
                    } else {
                        reloadPanel(leftPanel, icons, i18n, leftPanel.list.getCursor(), provMgr);
                        reloadPanel(rightPanel, icons, i18n, rightPanel.list.getCursor(), provMgr);
                        settingsScreen.close();
                    }
                }
            }
            break;
        }

        case InputLayer::WebSocketInstallerScreen: {
            WebSocketInstallerAction action = webSocketInstallerScreen.handleInput(kDown, &tap);
            if (action == WebSocketInstallerAction::Close)
                webSocketInstallerScreen.close();
            break;
        }

        case InputLayer::NetworkDriveForm: {
            NetworkDriveFormAction formAction = networkDriveForm.handleInput(kDown, i18n, &tap);
            if (formAction == NetworkDriveFormAction::Close) {
                networkDriveForm.close();
            } else if (formAction == NetworkDriveFormAction::Save) {
                const config::NetworkDriveConfig& cfg = networkDriveForm.result();
                if (cfg.name.empty()) {
                    toast.show(i18n.t("network_form.error_name_required"), "", ToastKind::Warning, 3000);
                    break;
                }
                if (cfg.address.empty()) {
                    toast.show(i18n.t("network_form.error_address_required"), "", ToastKind::Warning, 3000);
                    break;
                }

                std::shared_ptr<fs::FileProvider> prov;
                std::string providerErr;
                if (cfg.type == config::NetworkDriveType::WebDAV) {
                    prov = std::make_shared<fs::WebDavProvider>(
                        cfg.id, cfg.name, cfg.address, cfg.username, cfg.password);
                } else if (cfg.type == config::NetworkDriveType::SMB2) {
                    std::string server;
                    std::string share;
                    if (!parseSmbAddress(cfg.address, server, share)) {
                        providerErr = "SMB address must be server/share";
                    } else {
                        prov = std::make_shared<fs::SmbProvider>(
                            cfg.id, cfg.name, server, share, cfg.username, cfg.password);
                    }
                }

                if (!providerErr.empty()) {
                    toast.show(i18n.t("error.operation_failed"), providerErr.c_str(),
                               ToastKind::Error, 3200);
                    break;
                }
                if (!prov) {
                    toast.show(i18n.t("error.operation_failed"), "Failed to create provider",
                               ToastKind::Error, 3200);
                    break;
                }

                bool editingDrive = networkDriveForm.isEditing();
                config::AppConfig nextConfig = appConfig;
                if (editingDrive) {
                    bool updated = false;
                    for (auto& d : nextConfig.networkDrives) {
                        if (d.id == cfg.id) {
                            d = cfg;
                            updated = true;
                            break;
                        }
                    }
                    if (!updated)
                        nextConfig.networkDrives.push_back(cfg);
                } else {
                    nextConfig.networkDrives.push_back(cfg);
                }

                std::string saveErr;
                if (!config::saveConfig(configPath, nextConfig, saveErr)) {
                    toast.show(i18n.t("error.operation_failed"), saveErr.c_str(),
                               ToastKind::Error, 3200);
                    break;
                }

                if (editingDrive)
                    provMgr.removeProvider(cfg.id);

                provMgr.registerProvider(prov);
                appConfig = std::move(nextConfig);

                if (fs::ProviderManager::isVirtualRoot(leftPanel.path))
                    reloadPanel(leftPanel, icons, i18n, leftPanel.list.getCursor(), provMgr);
                if (fs::ProviderManager::isVirtualRoot(rightPanel.path))
                    reloadPanel(rightPanel, icons, i18n, rightPanel.list.getCursor(), provMgr);

                networkDriveForm.close();
                toast.show(i18n.t(editingDrive ? "toast.drive_updated" : "toast.drive_added"),
                           "", ToastKind::Success, 2200);
            }
            break;
        }

        case InputLayer::InstallerScreen: {
            installerScreen.update();
            InstallerAction action = installerScreen.handleInput(kDown, &tap);
            if (action == InstallerAction::Close) {
                if (installerScreen.shouldRefreshOnClose())
                    refreshInstallAffectedDirs(installerScreen.sourceDirectories());
                installerScreen.close();
                pendingInstallItems.clear();
            }
            break;
        }

        case InputLayer::ModalConfirm: {
            ConfirmResult cr = modalConfirm.handleInput(kDown, &tap);
            if (cr == ConfirmResult::Confirmed) {
                if (pendingConfirm == PendingConfirm::DeleteItems) {
                    modalConfirm.close();
                    pendingConfirm = PendingConfirm::None;
                    interrupted = false;
                    modalProgress.open(i18n.t("progress.deleting"), "");
                    fs::TransferResult deleteResult = provMgr.deleteEntries(
                        pendingDeletePaths,
                        makeTransferCallbacks(fs::TransferOperation::Delete));
                    modalProgress.close();
                    refreshPath(savedDeleteDir, savedDeleteCursor);
                    pendingDeletePaths.clear();
                    if (deleteResult.interrupted) {
                        toast.show(i18n.t("toast.interrupted"), "", ToastKind::Warning, 4000);
                        clipboard.clear();
                    } else if (deleteResult.aborted)
                        toast.show(i18n.t("error.operation_failed"),
                                   deleteResult.lastError.c_str(), ToastKind::Error, 3000);
                    else if (deleteResult.ignoredErrors)
                        toast.show(i18n.t("toast.delete_had_errors"), "",
                                   ToastKind::Warning, 3200);
                    else
                        toast.show(i18n.t("toast.deleted"), "", ToastKind::Success, 2200);
                }
                if (pendingConfirm == PendingConfirm::DeleteDrive) {
                    modalConfirm.close();
                    std::string driveId = pendingDeletePaths.empty() ? "" : pendingDeletePaths[0];
                    pendingConfirm = PendingConfirm::None;
                    pendingDeletePaths.clear();
                    if (!driveId.empty()) {
                        // Get display prefix before removing the provider
                        std::string deletedDisplayPrefix;
                        auto* driveProv = provMgr.findProvider(driveId);
                        if (driveProv)
                            deletedDisplayPrefix = driveProv->displayPrefix() + "/";

                        provMgr.removeProvider(driveId);
                        appConfig.networkDrives.erase(
                            std::remove_if(appConfig.networkDrives.begin(),
                                           appConfig.networkDrives.end(),
                                           [&](const config::NetworkDriveConfig& d) {
                                               return d.id == driveId;
                                           }),
                            appConfig.networkDrives.end());
                        std::string saveErr;
                        config::saveConfig(configPath, appConfig, saveErr);
                        // If any panel was inside this drive or at virtual root, refresh/navigate
                        bool leftAffected = fs::ProviderManager::isVirtualRoot(leftPanel.path)
                            || (!deletedDisplayPrefix.empty() &&
                                leftPanel.path.rfind(deletedDisplayPrefix, 0) == 0);
                        bool rightAffected = fs::ProviderManager::isVirtualRoot(rightPanel.path)
                            || (!deletedDisplayPrefix.empty() &&
                                rightPanel.path.rfind(deletedDisplayPrefix, 0) == 0);
                        if (leftAffected)
                            navigatePanel(leftPanel, "/", icons, i18n, provMgr);
                        if (rightAffected)
                            navigatePanel(rightPanel, "/", icons, i18n, provMgr);
                        toast.show(i18n.t("toast.drive_deleted"), "", ToastKind::Success, 2200);
                    }
                }
                if (pendingConfirm == PendingConfirm::UnmountDrive) {
                    modalConfirm.close();
                    std::string driveId = pendingDeletePaths.empty() ? "" : pendingDeletePaths[0];
                    pendingConfirm = PendingConfirm::None;
                    pendingDeletePaths.clear();
                    if (!driveId.empty()) {
                        std::string err;
                        if (!usbDriveManager.unmountByProviderId(driveId, err)) {
                            toast.show(i18n.t("error.operation_failed"), err.c_str(),
                                       ToastKind::Error, 3200);
                        } else {
                            applyUsbSnapshot(usbDriveManager.refreshNow(), false);
                            toast.show(i18n.t("toast.usb_unmounted"), "",
                                       ToastKind::Success, 2200);
                        }
                    }
                }
            } else if (cr == ConfirmResult::Cancelled) {
                modalConfirm.close();
                pendingConfirm = PendingConfirm::None;
                pendingDeletePaths.clear();
            }
            break;
        }

        case InputLayer::ModalChoice: {
            ChoiceResult ch = modalChoice.handleInput(kDown, &tap);
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
                    std::vector<fs::TransferEntry> transferEntries;
                    transferEntries.reserve(clipboard.items().size());
                    for (const auto& e : clipboard.items())
                        transferEntries.push_back({e.fullPath, fs::joinPath(destDir, e.name)});
                    fs::TransferStrategy strategy = ch == ChoiceResult::Merge
                        ? fs::TransferStrategy::Merge
                        : fs::TransferStrategy::Overwrite;
                    fs::TransferResult transferResult = provMgr.transferEntries(
                        transferEntries,
                        isCut ? fs::TransferOperation::Move : fs::TransferOperation::Copy,
                        strategy, makeTransferCallbacks(isCut ? fs::TransferOperation::Move
                                                              : fs::TransferOperation::Copy));
                    modalProgress.close();
                    clipboard.clear();
                    activeList.clearSelection();
                    refreshPath(destDir, savedCursor);
                    if (transferResult.interrupted)
                        toast.show(i18n.t("toast.interrupted"), "", ToastKind::Warning, 4000);
                    else if (transferResult.aborted)
                        toast.show(i18n.t("error.operation_failed"),
                                   transferResult.lastError.c_str(), ToastKind::Error, 3000);
                    else if (transferResult.ignoredErrors)
                        toast.show(i18n.t(isCut ? "toast.move_had_errors" : "toast.copy_had_errors"),
                                   "", ToastKind::Warning, 3200);
                    else
                        toast.show(i18n.t(isCut ? "toast.moved" : "toast.copied"), "", ToastKind::Success, 2200);
                }
            }
            break;
        }

        case InputLayer::ModalInfo:
            if (modalInfo.handleInput(kDown, &tap) != ConfirmResult::None) modalInfo.close();
            break;

        case InputLayer::ModalInstallPrompt: {
            InstallPromptResult result = modalInstallPrompt.handleInput(kDown, &tap);
            if (result != InstallPromptResult::None) {
                modalInstallPrompt.close();
                if (result == InstallPromptResult::Install ||
                    result == InstallPromptResult::InstallAndDelete) {
                    auto sourceCallbacks = std::make_shared<InstallDataSourceCallbacks>();
                    sourceCallbacks->readRange =
                        [&provMgr](const InstallQueueItem& item, uint64_t offset, size_t size,
                                   void* outBuffer, std::string& errOut) {
                            return provMgr.readFile(item.path, offset, size, outBuffer, errOut);
                        };
                    installerScreen.open(
                        pendingInstallItems,
                        result == InstallPromptResult::InstallAndDelete
                            ? InstallDeleteMode::DeleteAfterInstall
                            : InstallDeleteMode::KeepFiles,
                        appletMode, sourceCallbacks);
                    activeList.clearSelection();
                } else {
                    pendingInstallItems.clear();
                }
            }
            break;
        }

        case InputLayer::MainMenu:
            if (kDown & HidNpadButton_Plus) {
                mainMenu.close();
            } else {
                updateMainMenu = true;
            }
            break;

        case InputLayer::BasePanels: {
            bool consumed = false;
            uint64_t baseKDown = kDown;

            if (appConfig.touchButtonsEnabled && tap.active) {
                FooterTouchButtonsLayout touchLayout = footerTouchButtonsLayout(true);
                if (pointInRect(&tap, touchLayout.prevX, touchLayout.y, touchLayout.pageW,
                                touchLayout.h)) {
                    baseKDown |= HidNpadButton_AnyLeft;
                } else if (pointInRect(&tap, touchLayout.nextX, touchLayout.y,
                                       touchLayout.pageW, touchLayout.h)) {
                    baseKDown |= HidNpadButton_AnyRight;
                } else if (pointInRect(&tap, touchLayout.selectAllX, touchLayout.y,
                                       touchLayout.selectAllW, touchLayout.h)) {
                    baseKDown |= HidNpadButton_X;
                } else if (pointInRect(&tap, touchLayout.selectX, touchLayout.y,
                                       touchLayout.selectW, touchLayout.h)) {
                    baseKDown |= HidNpadButton_Y;
                }
            }

            if (tap.active && tap.y >= 0 && tap.y < theme::HEADER_H) {
                HeaderIconLayout headerIcons = headerIconLayout();
                if (tap.x >= headerIcons.menuHitX &&
                    tap.x < (headerIcons.menuHitX + headerIcons.hitW) &&
                    tap.y >= headerIcons.hitY &&
                    tap.y < (headerIcons.hitY + headerIcons.hitH)) {
                    mainMenu.open();
                    consumed = true;
                } else if (tap.x >= headerIcons.aboutHitX &&
                           tap.x < (headerIcons.aboutHitX + headerIcons.hitW) &&
                           tap.y >= headerIcons.hitY &&
                           tap.y < (headerIcons.hitY + headerIcons.hitH)) {
                    modalInfo.open(i18n.t("menu.about"),
                                   localizedDocText("about", "help.about_body"),
                                   theme::FONT_SIZE_ITEM);
                    consumed = true;
                }
            }

            if (!consumed && tap.active) {
                int panelY = theme::HEADER_H;
                int panelH = theme::PANEL_CONTENT_H;
                float lwf = anim.currentLeftW();
                int lw = static_cast<int>(lwf);
                int rw = theme::SCREEN_W - lw;
                int rx = lw;
                int activeX = activePanel == PANEL_LEFT ? 0 : rx;
                int activeW = activePanel == PANEL_LEFT ? lw : rw;

                if (tap.y >= panelY && tap.y < (panelY + panelH)) {
                    if (tap.x >= activeX && tap.x < (activeX + activeW)) {
                        int hitIndex = activeList.hitTestIndex(tap.y - panelY);
                        if (hitIndex >= 0) {
                            if (hitIndex == activeList.getCursor()) {
                                baseKDown |= HidNpadButton_A;
                            } else {
                                activeList.setCursor(hitIndex);
                                consumed = true;
                            }
                        }
                    } else if (!anim.isAnimating()) {
                        if (activePanel == PANEL_LEFT && tap.x >= rx && tap.x < (rx + rw)) {
                            activePanel = PANEL_RIGHT;
                            anim.start(anim.currentLeftW(),
                                       static_cast<float>(theme::INACTIVE_PANEL_W));
                            consumed = true;
                        } else if (activePanel == PANEL_RIGHT && tap.x >= 0 && tap.x < lw) {
                            activePanel = PANEL_LEFT;
                            anim.start(anim.currentLeftW(),
                                       static_cast<float>(theme::ACTIVE_PANEL_W));
                            consumed = true;
                        }
                    }
                }
            }

            if (consumed)
                break;

            if (baseKDown & HidNpadButton_Plus) {
                mainMenu.open();
                break;
            }

            if (!anim.isAnimating()) {
                if ((baseKDown & HidNpadButton_L) && activePanel != PANEL_LEFT) {
                    activePanel = PANEL_LEFT;
                    anim.start(anim.currentLeftW(), static_cast<float>(theme::ACTIVE_PANEL_W));
                }
                if ((baseKDown & HidNpadButton_R) && activePanel != PANEL_RIGHT) {
                    activePanel = PANEL_RIGHT;
                    anim.start(anim.currentLeftW(), static_cast<float>(theme::INACTIVE_PANEL_W));
                }
            }

            if (baseKDown & HidNpadButton_AnyUp) activeList.moveCursorUp();
            if (baseKDown & HidNpadButton_AnyDown) activeList.moveCursorDown();
            if (baseKDown & HidNpadButton_AnyLeft) activeList.moveCursorPageUp(pageItems);
            if (baseKDown & HidNpadButton_AnyRight) activeList.moveCursorPageDown(pageItems);

            if (baseKDown & HidNpadButton_A) {
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
                            if (fs::ProviderManager::isVirtualRoot(active.path))
                                target = item->label + "/";
                            else
                                target = fs::joinPath(active.path, item->label);
                            navigateToPath(activePanel, target);
                        }
                    } else if (item->action == ACTION_GO_UP) {
                        navigateToPath(activePanel, fs::parentPath(active.path));
                    } else if (item->action == ACTION_WEBSOCKET_INSTALLER) {
                        activeList.clearSelection();
                        webSocketInstallerScreen.open(i18n);
                    } else if (item->action == ACTION_ADD_NETWORK_DRIVE) {
                        activeList.clearSelection();
                        networkDriveForm.openNew();
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

            if (baseKDown & HidNpadButton_B) {
                if (!fs::ProviderManager::isVirtualRoot(active.path))
                    navigateToPath(activePanel, fs::parentPath(active.path));
            }

            if (baseKDown & HidNpadButton_Y) activeList.toggleSelect();
            if (baseKDown & HidNpadButton_X) activeList.toggleSelectAll();
            break;
        }
        }

        MainMenuState menuSt;
        fillMenuState(menuSt);

        if (updateMainMenu)
            mainMenu.update(delta, kDown, menuInputState, &tap);

        if (mainMenu.isOpen()) {
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
                settingsScreen.open(appConfig.language, appConfig.touchButtonsEnabled);
                mainMenu.close();
                break;
            case MenuCommand::Help:
                modalInfo.open(i18n.t("menu.help"),
                               localizedDocText("help", "help.short"),
                               theme::FONT_SIZE_ITEM);
                mainMenu.close();
                break;
            case MenuCommand::About:
                modalInfo.open(i18n.t("menu.about"),
                               localizedDocText("about", "help.about_body"),
                               theme::FONT_SIZE_ITEM);
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
            case MenuCommand::InstallApplications: {
                std::vector<InstallQueueItem> items = installItemsForMenu(active);
                if (items.empty()) {
                    mainMenu.close();
                    break;
                }
                mainMenu.close();
                startInstaller(std::move(items));
                break;
            }
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
                    if (provMgr.pathExists(fs::joinPath(active.path, e.name))) { anyExists = true; break; }
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
                    std::vector<fs::TransferEntry> transferEntries;
                    transferEntries.reserve(clipboard.items().size());
                    for (const auto& e : clipboard.items())
                        transferEntries.push_back({e.fullPath, fs::joinPath(destDir, e.name)});
                    fs::TransferResult transferResult = provMgr.transferEntries(
                        transferEntries,
                        isCut ? fs::TransferOperation::Move : fs::TransferOperation::Copy,
                        fs::TransferStrategy::Simple,
                        makeTransferCallbacks(isCut ? fs::TransferOperation::Move
                                                    : fs::TransferOperation::Copy));
                    modalProgress.close();
                    clipboard.clear();
                    activeList.clearSelection();
                    refreshPath(destDir, savedCursor);
                    if (transferResult.interrupted)
                        toast.show(i18n.t("toast.interrupted"), "", ToastKind::Warning, 4000);
                    else if (transferResult.aborted)
                        toast.show(i18n.t("error.operation_failed"),
                                   transferResult.lastError.c_str(), ToastKind::Error, 3000);
                    else if (transferResult.ignoredErrors)
                        toast.show(i18n.t(isCut ? "toast.move_had_errors" : "toast.copy_had_errors"),
                                   "", ToastKind::Warning, 3200);
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
                if (provMgr.pathExists(full)) {
                    toast.show(i18n.t("error.exists"), "", ToastKind::Warning, 3000);
                    break;
                }
                std::string err;
                if (!provMgr.createDirectory(full, err)) {
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
                if (provMgr.pathExists(to)) {
                    toast.show(i18n.t("error.exists"), "", ToastKind::Warning, 3000);
                    break;
                }
                std::string err;
                if (!provMgr.renamePath(from, to, err)) {
                    toast.show(i18n.t("error.operation_failed"), err.c_str(), ToastKind::Error, 3000);
                    break;
                }
                int kc = activeList.getCursor();
                refreshPath(active.path, kc);
                mainMenu.close();
                break;
            }
            case MenuCommand::EditDrive: {
                const ListItem* cur = activeList.getSelectedItem();
                auto* driveProv = providerForRootItem(cur);
                if (!driveProv || !isNetworkProvider(driveProv)) break;
                for (const auto& d : appConfig.networkDrives) {
                    if (d.id == driveProv->providerId()) {
                        networkDriveForm.openEdit(d);
                        break;
                    }
                }
                mainMenu.close();
                break;
            }
            case MenuCommand::DeleteDrive: {
                const ListItem* cur = activeList.getSelectedItem();
                auto* driveProv = providerForRootItem(cur);
                if (!driveProv || !isNetworkProvider(driveProv)) break;
                pendingDeletePaths.clear();
                pendingDeletePaths.push_back(driveProv->providerId());
                savedDeleteDir    = active.path;
                savedDeleteCursor = activeList.getCursor();
                modalConfirm.open(i18n.t("confirm.delete_drive_title"),
                                  i18n.t("confirm.delete_drive_body"));
                pendingConfirm = PendingConfirm::DeleteDrive;
                mainMenu.close();
                break;
            }
            case MenuCommand::UnmountDrive: {
                const ListItem* cur = activeList.getSelectedItem();
                auto* driveProv = providerForRootItem(cur);
                if (!driveProv || !isUsbProvider(driveProv)) break;
                pendingDeletePaths.clear();
                pendingDeletePaths.push_back(driveProv->providerId());
                modalConfirm.open(i18n.t("confirm.unmount_drive_title"),
                                  i18n.t("confirm.unmount_drive_body"));
                pendingConfirm = PendingConfirm::UnmountDrive;
                mainMenu.close();
                break;
            }
            default: break;
            }
        }

        toast.update(delta);
        loadingOverlay.update(delta);
        anim.update(delta);

        if (pendingPanelLoad.active) {
            bool ready = false;
            std::vector<ListItem> loadedItems;
            std::string loadErr;
            ActivePanel loadPanel = pendingPanelLoad.panel;
            std::string loadPath = pendingPanelLoad.path;
            {
                std::lock_guard<std::mutex> lock(pendingPanelLoad.mutex);
                ready = pendingPanelLoad.finished;
                if (ready) {
                    loadedItems = std::move(pendingPanelLoad.items);
                    loadErr = std::move(pendingPanelLoad.err);
                }
            }

            if (ready) {
                pendingPanelLoad.join();
                pendingPanelLoad.active = false;
                loadingOverlay.hide();

                if (!loadErr.empty()) {
#ifdef XPLORE_DEBUG
                    std::printf("[nav] async load failed path=%s err=%s\n", loadPath.c_str(),
                                loadErr.c_str());
#endif
                    toast.show(i18n.t("error.operation_failed"), loadErr.c_str(),
                               ToastKind::Error, 3200);
                } else {
#ifdef XPLORE_DEBUG
                    std::printf("[nav] async load done path=%s count=%zu\n", loadPath.c_str(),
                                loadedItems.size());
#endif
                    PanelState& targetPanel = panelFor(loadPanel);
                    bool hasGoUp = !fs::ProviderManager::isVirtualRoot(loadPath);
                    bool allowSel = provMgr.pathAllowsSelection(loadPath);
                    targetPanel.path = loadPath;
                    targetPanel.list.clearSelection();
                    targetPanel.list.setItems(std::move(loadedItems), hasGoUp, allowSel);
                }
            }
        }

        leftPanel.list.updateCache(renderer, fontManager, theme::ACTIVE_PANEL_W,
                                   theme::PANEL_CONTENT_H);
        rightPanel.list.updateCache(renderer, fontManager, theme::ACTIVE_PANEL_W,
                                    theme::PANEL_CONTENT_H);

        renderScene();

        const bool anyOverlay =
            mainMenu.isOpen() || modalConfirm.isOpen() || modalChoice.isOpen()
            || modalProgress.isOpen() || modalInfo.isOpen() || modalInstallPrompt.isOpen()
            || networkDriveForm.isOpen() || loadingOverlay.isVisible();
        if (anyOverlay && !imageViewer.isOpen() && !installerScreen.isOpen()
            && !settingsScreen.isOpen()
            && !webSocketInstallerScreen.isOpen()) {
            int scrimH = theme::HEADER_H + theme::PANEL_CONTENT_H;
            renderer.drawRectFilled(0, 0, theme::SCREEN_W, scrimH, theme::MENU_SCRIM_CONTENT);
        }
        if (!imageViewer.isOpen() && !installerScreen.isOpen()
            && !settingsScreen.isOpen()
            && !webSocketInstallerScreen.isOpen()
            && !networkDriveForm.isOpen())
            renderFooter(renderer, fontManager, i18n, appConfig.touchButtonsEnabled);

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
        networkDriveForm.render(renderer, fontManager, i18n);
        loadingOverlay.render(renderer, fontManager, i18n);

        toast.render(renderer, fontManager);
        renderer.present();

        if (appQuit) break;
    }

    leftPanel.list.destroyCache();
    rightPanel.list.destroyCache();
    webSocketInstallerScreen.close();
    usbDriveManager.shutdown();
    // Destroy network providers before socket services are torn down.
    provMgr = fs::ProviderManager();
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

} // namespace xxplore
