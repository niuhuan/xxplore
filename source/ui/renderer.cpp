#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include <SDL_image.h>
#include <cstdio>

namespace xplore {

bool Renderer::init() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    sdlInited = true;

    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);

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
    IMG_Quit();
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

SDL_Texture* Renderer::loadTexture(const char* path) {
    SDL_Surface* surf = IMG_Load(path);
    if (!surf) {
        printf("IMG_Load failed (%s): %s\n", path, IMG_GetError());
        return nullptr;
    }
    SDL_Texture* tex = SDL_CreateTextureFromSurface(sdlRenderer, surf);
    SDL_FreeSurface(surf);
    if (!tex)
        printf("SDL_CreateTextureFromSurface failed: %s\n", SDL_GetError());
    return tex;
}

void Renderer::drawTexture(SDL_Texture* tex, int x, int y, int w, int h) {
    if (!tex) return;
    SDL_Rect dst = {x, y, w, h};
    SDL_RenderCopy(sdlRenderer, tex, nullptr, &dst);
}

} // namespace xplore
