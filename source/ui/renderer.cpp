#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include <cstdio>

namespace xplore {

bool Renderer::init() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    sdlInited = true;

    sdlWindow = SDL_CreateWindow("xplore",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        theme::SCREEN_W, theme::SCREEN_H, 0);
    if (!sdlWindow) {
        printf("SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    sdlRenderer = SDL_CreateRenderer(sdlWindow, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!sdlRenderer) {
        printf("SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_SetRenderDrawBlendMode(sdlRenderer, SDL_BLENDMODE_BLEND);
    return true;
}

void Renderer::shutdown() {
    if (sdlRenderer) { SDL_DestroyRenderer(sdlRenderer); sdlRenderer = nullptr; }
    if (sdlWindow)   { SDL_DestroyWindow(sdlWindow);     sdlWindow   = nullptr; }
    if (sdlInited)   { SDL_Quit();                        sdlInited   = false;   }
}

void Renderer::clear(SDL_Color c) {
    SDL_SetRenderDrawColor(sdlRenderer, c.r, c.g, c.b, c.a);
    SDL_RenderClear(sdlRenderer);
}

void Renderer::present() {
    SDL_RenderPresent(sdlRenderer);
}

void Renderer::drawRectFilled(int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(sdlRenderer, c.r, c.g, c.b, c.a);
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderFillRect(sdlRenderer, &rect);
}

void Renderer::drawRect(int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(sdlRenderer, c.r, c.g, c.b, c.a);
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderDrawRect(sdlRenderer, &rect);
}

void Renderer::drawLine(int x1, int y1, int x2, int y2, SDL_Color c) {
    SDL_SetRenderDrawColor(sdlRenderer, c.r, c.g, c.b, c.a);
    SDL_RenderDrawLine(sdlRenderer, x1, y1, x2, y2);
}

void Renderer::setClipRect(int x, int y, int w, int h) {
    SDL_Rect clip = {x, y, w, h};
    SDL_RenderSetClipRect(sdlRenderer, &clip);
}

void Renderer::clearClipRect() {
    SDL_RenderSetClipRect(sdlRenderer, nullptr);
}

} // namespace xplore
