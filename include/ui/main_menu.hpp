#pragma once
#include <SDL.h>
#include <cstdint>
#include <string>

namespace xplore {

class Renderer;
class FontManager;
class I18n;

enum class MenuCommand {
    None,
    CloseMenu,
    ToggleSelectMode,
    Rename,
    NewFolder,
    Delete,
    Copy,
    Cut,
    Paste,
    ViewClipboard,
    ClearClipboard,
    InstallApplications,
    Settings,
    Help,
    About,
    ExitApp,
    EditDrive,
    DeleteDrive,
};

/// Per-frame UI state for the bottom sheet (filled by Application).
struct MainMenuState {
    std::string contextLine;
    bool        pasteFromCut = false;

    bool disableSelectToggle = false;
    bool disableRename       = false;
    bool disableNewFolder    = false;
    bool disableDelete       = false;
    bool disableCopy         = false;
    bool disableCut          = false;
    bool disablePaste        = false;
    bool disableViewClip     = false;
    bool disableClearClip    = false;
    bool disableInstall      = false;
    bool disableSettings     = false;
    bool disableHelp         = false;
    bool disableAbout        = false;
    bool disableExit         = false;

    /// When true, Rename cell shows "Edit" label and fires EditDrive command.
    bool renameIsEdit        = false;
    /// When true, Delete cell fires DeleteDrive command (for network drives in root).
    bool deleteIsDriveDel    = false;
};

/// Bottom-anchored 4-column grid main menu with slide animation.
class BottomMainMenu {
public:
    void open();
    void close();
    bool isOpen() const { return open_; }

    void update(uint32_t deltaMs, uint64_t kDown, const MainMenuState& st);
    void render(Renderer& renderer, FontManager& fm, const I18n& i18n,
                const MainMenuState& st);

    /// Last activated command; cleared when read.
    MenuCommand takeCommand();

private:
    bool  open_     = false;
    float anim_     = 1.0f; ///< 0 = hidden, 1 = visible
    int   focusRow_ = 1;
    int   focusCol_ = 0;

    MenuCommand pending_ = MenuCommand::None;

    void moveFocusVertical(int delta, const MainMenuState& st);
    void moveFocusHorizontal(int delta, const MainMenuState& st);
    void activateCell(const MainMenuState& st);
    bool tryFocusAt(int r, int c, const MainMenuState& st);

    bool cellDisabled(int row, int col, const MainMenuState& st) const;
    MenuCommand cmdAt(int row, int col, const MainMenuState& st) const;
};

} // namespace xplore
