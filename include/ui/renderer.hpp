#pragma once
#include <SDL.h>

namespace xplore {

class Renderer {
public:
    bool init();
    void shutdown();

    void clear(SDL_Color color);
    void present();
    void drawRectFilled(int x, int y, int w, int h, SDL_Color color);
    void drawRect(int x, int y, int w, int h, SDL_Color color);
    void drawLine(int x1, int y1, int x2, int y2, SDL_Color color);
    void setClipRect(int x, int y, int w, int h);
    void clearClipRect();

    SDL_Renderer* sdl() const { return sdlRenderer; }

private:
    SDL_Window*   sdlWindow   = nullptr;
    SDL_Renderer* sdlRenderer = nullptr;
    bool sdlInited = false;
};

} // namespace xplore
