#pragma once
#include "util/app_config.hpp"
#include <cstdint>
#include <string>

namespace xplore {

class Renderer;
class FontManager;
class I18n;

enum class NetworkDriveFormAction { None, Close, Save };

/// Full-screen form for adding/editing a network drive.
class NetworkDriveForm {
public:
    /// Open form for creating a new drive.
    void openNew();

    /// Open form for editing an existing drive.
    void openEdit(const config::NetworkDriveConfig& existing);

    void close();
    bool isOpen() const { return open_; }

    /// The resulting config (valid after Save).
    const config::NetworkDriveConfig& result() const { return config_; }

    /// True if editing an existing drive (vs creating new).
    bool isEditing() const { return editing_; }

    NetworkDriveFormAction handleInput(uint64_t kDown, const I18n& i18n);
    void render(Renderer& renderer, FontManager& fm, const I18n& i18n) const;

private:
    bool open_ = false;
    bool editing_ = false;
    config::NetworkDriveConfig config_;

    // Focus: 0=Name, 1=Type, 2=Address, 3=Username, 4=Password, 5=Back, 6=Save
    int focusRow_ = 0;
    static constexpr int kRowName     = 0;
    static constexpr int kRowType     = 1;
    static constexpr int kRowAddress  = 2;
    static constexpr int kRowUsername = 3;
    static constexpr int kRowPassword = 4;
    static constexpr int kRowButtons  = 5;
    int buttonFocusCol_ = 0; // 0=Back, 1=Save

    void moveVertical(int delta);
    void moveHorizontal(int delta);
    void activateRow(const I18n& i18n);
};

} // namespace xplore
