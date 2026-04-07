#pragma once
#include <SDL.h>
#include <SDL_ttf.h>
#include <switch.h>
#include <unordered_map>

namespace xplore {

class FontManager {
public:
    bool init();
    void shutdown();

    void drawText(SDL_Renderer* renderer, const char* text,
                  int x, int y, int fontSize, SDL_Color color);
    int measureText(const char* text, int fontSize);
    int fontHeight(int fontSize);

private:
    TTF_Font* getFont(int size);

    PlFontData sharedFontData = {};
    std::unordered_map<int, TTF_Font*> fontCache;
    bool ttfInited = false;
    bool plInited  = false;
};

} // namespace xplore
