#pragma once

#include <SDL.h>

namespace xxplore {

class Renderer;
class FontManager;

namespace ui {

constexpr int kPanelTitleBarH = 54;
constexpr int kPanelTitleFontSize = 24;
constexpr int kPanelCloseButtonSize = 34;

void drawPanelCloseButton(Renderer& renderer, int cardX, int cardY, int cardW, bool closeFocused);

void drawPanelTitleBar(Renderer& renderer, FontManager& fm, int x, int y, int w,
                       const char* title, bool showCloseButton, bool closeFocused);

bool panelCloseButtonHit(int cardX, int cardY, int cardW, int tapX, int tapY);

} // namespace ui
} // namespace xxplore
