#include "ui/font_manager.hpp"
#include <cstring>
#include <cstdio>
#include <string>

namespace xplore {

namespace {

constexpr char kMissingGlyphPlaceholder = '?';

bool decodeUtf8Codepoint(const char*& p, Uint32& codepoint) {
    const unsigned char* s = reinterpret_cast<const unsigned char*>(p);
    if (!s || *s == 0)
        return false;

    if (s[0] < 0x80) {
        codepoint = s[0];
        p += 1;
        return true;
    }

    if ((s[0] & 0xE0) == 0xC0 && s[1] != 0) {
        codepoint = (static_cast<Uint32>(s[0] & 0x1F) << 6) |
                    static_cast<Uint32>(s[1] & 0x3F);
        p += 2;
        return true;
    }

    if ((s[0] & 0xF0) == 0xE0 && s[1] != 0 && s[2] != 0) {
        codepoint = (static_cast<Uint32>(s[0] & 0x0F) << 12) |
                    (static_cast<Uint32>(s[1] & 0x3F) << 6) |
                    static_cast<Uint32>(s[2] & 0x3F);
        p += 3;
        return true;
    }

    if ((s[0] & 0xF8) == 0xF0 && s[1] != 0 && s[2] != 0 && s[3] != 0) {
        codepoint = (static_cast<Uint32>(s[0] & 0x07) << 18) |
                    (static_cast<Uint32>(s[1] & 0x3F) << 12) |
                    (static_cast<Uint32>(s[2] & 0x3F) << 6) |
                    static_cast<Uint32>(s[3] & 0x3F);
        p += 4;
        return true;
    }

    codepoint = static_cast<Uint32>(*s);
    p += 1;
    return true;
}

void appendUtf8Codepoint(std::string& out, Uint32 codepoint) {
    if (codepoint < 0x80) {
        out.push_back(static_cast<char>(codepoint));
        return;
    }
    if (codepoint < 0x800) {
        out.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        return;
    }
    if (codepoint < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        return;
    }

    out.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
}

} // namespace

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

std::string FontManager::sanitizeText(TTF_Font* font, const char* text) {
    if (!font || !text || !text[0])
        return {};

    std::string sanitized;
    sanitized.reserve(std::strlen(text));
    const char* p = text;
    while (*p) {
        const char* original = p;
        Uint32 codepoint = 0;
        if (!decodeUtf8Codepoint(p, codepoint)) {
            sanitized.push_back(kMissingGlyphPlaceholder);
            break;
        }

        if (codepoint == '\r' || codepoint == '\n' || codepoint == '\t' ||
            TTF_GlyphIsProvided32(font, codepoint)) {
            appendUtf8Codepoint(sanitized, codepoint);
        } else {
            sanitized.push_back(kMissingGlyphPlaceholder);
        }

        if (p == original)
            ++p;
    }
    return sanitized;
}

void FontManager::drawText(SDL_Renderer* renderer, const char* text,
                           int x, int y, int fontSize, SDL_Color color) {
    if (!text || !text[0]) return;

    TTF_Font* font = getFont(fontSize);
    if (!font) return;

    std::string sanitized = sanitizeText(font, text);
    if (sanitized.empty()) return;

    SDL_Surface* surface = TTF_RenderUTF8_Blended(font, sanitized.c_str(), color);
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
    std::string sanitized = sanitizeText(font, text);
    if (sanitized.empty()) return 0;
    int w = 0, h = 0;
    TTF_SizeUTF8(font, sanitized.c_str(), &w, &h);
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
