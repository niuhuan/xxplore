#pragma once
#include "util/app_config.hpp"
#include "ui/touch_event.hpp"
#include <cstdint>

namespace xplore {

class Renderer;
class FontManager;
class I18n;

enum class SettingsAction { None, Close, Save };

class SettingsScreen {
public:
    void open(config::AppLanguage currentLanguage, bool touchButtonsEnabled);
    void close();

    bool isOpen() const { return open_; }
    config::AppLanguage selectedLanguage() const { return selectedLanguage_; }
    bool touchButtonsEnabled() const { return touchButtonsEnabled_; }

    SettingsAction handleInput(uint64_t kDown, const TouchTap* tap = nullptr);
    void render(Renderer& renderer, FontManager& fm, const I18n& i18n) const;

private:
    int languageIndex(int col) const;
    void moveHorizontal(int delta);
    void moveVertical(int delta);

    bool open_ = false;
    config::AppLanguage currentLanguage_;
    config::AppLanguage selectedLanguage_;
    bool touchButtonsEnabled_ = false;
    int focusRow_ = 0; // 0 language row, 1 touch buttons row, 2 button row
    int languageFocusCol_ = 0;
    int touchButtonsFocusCol_ = 0;
    int buttonFocusCol_ = 0;
};

} // namespace xplore
