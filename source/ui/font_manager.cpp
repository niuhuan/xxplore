#include "ui/font_manager.hpp"
#include <cstdio>
#include <string>

namespace xplore {

bool FontManager::init(const char* path) {
    if (TTF_Init() < 0) {
        printf("TTF_Init failed: %s\n", TTF_GetError());
        return false;
    }
    ttfInited = true;
    fontPath = path;

    // Verify the font file is loadable at a test size
    TTF_Font* test = TTF_OpenFont(fontPath.c_str(), 16);
    if (!test) {
        printf("Cannot open font %s: %s\n", fontPath.c_str(), TTF_GetError());
        return false;
    }
    fontCache[16] = test;
    return true;
}

void FontManager::shutdown() {
    for (auto& [size, font] : fontCache)
        TTF_CloseFont(font);
    fontCache.clear();

    if (ttfInited) { TTF_Quit(); ttfInited = false; }
}

TTF_Font* FontManager::getFont(int size) {
    auto it = fontCache.find(size);
    if (it != fontCache.end())
        return it->second;

    TTF_Font* font = TTF_OpenFont(fontPath.c_str(), size);
    if (!font) {
        printf("TTF_OpenFont failed (size %d): %s\n", size, TTF_GetError());
        return nullptr;
    }

    fontCache[size] = font;
    return font;
}

void FontManager::drawText(SDL_Renderer* renderer, const char* text,
                           int x, int y, int fontSize, SDL_Color color) {
    if (!text || !text[0]) return;

    TTF_Font* font = getFont(fontSize);
    if (!font) return;

    SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text, color);
    if (!surface) return;

    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (texture) {
        SDL_Rect dst = {x, y, surface->w, surface->h};
        SDL_RenderCopy(renderer, texture, nullptr, &dst);
        SDL_DestroyTexture(texture);
    }
    SDL_FreeSurface(surface);
}

int FontManager::measureText(const char* text, int fontSize) {
    if (!text || !text[0]) return 0;
    TTF_Font* font = getFont(fontSize);
    if (!font) return 0;
    int w = 0, h = 0;
    TTF_SizeUTF8(font, text, &w, &h);
    return w;
}

int FontManager::fontHeight(int fontSize) {
    TTF_Font* font = getFont(fontSize);
    if (!font) return fontSize;
    return TTF_FontHeight(font);
}

void FontManager::drawTextEllipsis(SDL_Renderer* renderer, const char* text,
                                   int x, int y, int fontSize, SDL_Color color,
                                   int maxWidth) {
    if (!text || !text[0] || maxWidth <= 0) return;
    if (measureText(text, fontSize) <= maxWidth) {
        drawText(renderer, text, x, y, fontSize, color);
        return;
    }
    const char ell[] = "...";
    int ellW = measureText(ell, fontSize);
    if (ellW >= maxWidth) {
        drawText(renderer, ell, x, y, fontSize, color);
        return;
    }
    std::string s(text);
    while (!s.empty()) {
        s.pop_back();
        if (s.empty()) break;
        if (measureText(s.c_str(), fontSize) + ellW <= maxWidth) {
            s += ell;
            drawText(renderer, s.c_str(), x, y, fontSize, color);
            return;
        }
    }
    drawText(renderer, ell, x, y, fontSize, color);
}

} // namespace xplore
