#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include <SDL2/SDL2_gfxPrimitives.h>
#include <SDL_image.h>
#include <cstdio>

namespace xxplore {

bool Renderer::init() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    sdlInited = true;

    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);

    sdlWindow = SDL_CreateWindow("xxplore",
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
    SDL_SetRenderDrawBlendMode(sdlRenderer, SDL_BLENDMODE_BLEND);
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

void Renderer::drawRoundedRectFilled(int x, int y, int w, int h, int radius,
                                     SDL_Color c) {
    if (w <= 0 || h <= 0) return;
    int x2 = x + w - 1;
    int y2 = y + h - 1;
    if (radius < 0) radius = 0;
    roundedBoxRGBA(sdlRenderer, x, y, x2, y2, radius, c.r, c.g, c.b, c.a);
}

void Renderer::drawRoundedRect(int x, int y, int w, int h, int radius, SDL_Color c) {
    if (w <= 0 || h <= 0) return;
    int x2 = x + w - 1;
    int y2 = y + h - 1;
    if (radius < 0) radius = 0;
    roundedRectangleRGBA(sdlRenderer, x, y, x2, y2, radius, c.r, c.g, c.b, c.a);
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

SDL_Texture* Renderer::createRenderTarget(int w, int h) {
    SDL_Texture* tex = SDL_CreateTexture(sdlRenderer,
        SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, w, h);
    if (!tex) {
        printf("createRenderTarget failed: %s\n", SDL_GetError());
        return nullptr;
    }
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    return tex;
}

void Renderer::setRenderTarget(SDL_Texture* tex) {
    SDL_SetRenderTarget(sdlRenderer, tex);
}

void Renderer::resetRenderTarget() {
    SDL_SetRenderTarget(sdlRenderer, nullptr);
}

} // namespace xxplore
