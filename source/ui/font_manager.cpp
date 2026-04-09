#include "ui/font_manager.hpp"
#include <cstring>
#include <cstdio>
#include <string>

namespace xxplore {

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

void popLastUtf8Codepoint(std::string& text) {
    if (text.empty())
        return;

    std::size_t pos = text.size() - 1;
    while (pos > 0 && (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80)
        --pos;
    text.erase(pos);
}

Uint32 resolveGlyphCodepoint(TTF_Font* font, Uint32 codepoint) {
    if (codepoint == '\t')
        codepoint = ' ';

    if (TTF_GlyphIsProvided32(font, codepoint))
        return codepoint;

    if (TTF_GlyphIsProvided32(font, static_cast<Uint32>(kMissingGlyphPlaceholder)))
        return static_cast<Uint32>(kMissingGlyphPlaceholder);

    if (TTF_GlyphIsProvided32(font, static_cast<Uint32>(' ')))
        return static_cast<Uint32>(' ');

    return 0;
}

} // namespace

bool FontManager::init(const char* path, std::size_t cacheLimitBytes) {
    if (TTF_Init() < 0) {
        printf("TTF_Init failed: %s\n", TTF_GetError());
        return false;
    }
    ttfInited = true;
    fontPath = path;
    glyphCacheLimitBytes = cacheLimitBytes;
    glyphCacheBytes = 0;
    glyphCache.clear();
    glyphLru.clear();

    // Verify the font file is loadable at a test size
    TTF_Font* test = TTF_OpenFont(fontPath.c_str(), 16);
    if (!test) {
        printf("Cannot open font %s: %s\n", fontPath.c_str(), TTF_GetError());
        TTF_Quit();
        ttfInited = false;
        return false;
    }
    fontCache[16] = test;
    return true;
}

void FontManager::shutdown() {
    for (auto& [key, glyph] : glyphCache) {
        if (glyph.texture)
            SDL_DestroyTexture(glyph.texture);
    }
    glyphCache.clear();
    glyphLru.clear();
    glyphCacheBytes = 0;

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

uint64_t FontManager::glyphKey(int size, Uint32 codepoint) const {
    return (static_cast<uint64_t>(static_cast<uint32_t>(size)) << 32) |
           static_cast<uint64_t>(codepoint);
}

void FontManager::touchGlyph(uint64_t key, GlyphEntry& glyph) {
    glyphLru.erase(glyph.lruIt);
    glyphLru.push_front(key);
    glyph.lruIt = glyphLru.begin();
}

void FontManager::evictGlyphs() {
    while (glyphCacheLimitBytes > 0 && glyphCacheBytes > glyphCacheLimitBytes &&
           !glyphLru.empty()) {
        uint64_t key = glyphLru.back();
        glyphLru.pop_back();

        auto it = glyphCache.find(key);
        if (it == glyphCache.end())
            continue;

        if (it->second.texture)
            SDL_DestroyTexture(it->second.texture);
        if (glyphCacheBytes >= it->second.bytes)
            glyphCacheBytes -= it->second.bytes;
        else
            glyphCacheBytes = 0;
        glyphCache.erase(it);
    }
}

FontManager::GlyphEntry* FontManager::getGlyph(SDL_Renderer* renderer, TTF_Font* font,
                                               int fontSize, Uint32 codepoint) {
    if (!renderer || !font)
        return nullptr;

    Uint32 resolvedCodepoint = resolveGlyphCodepoint(font, codepoint);
    if (resolvedCodepoint == 0)
        return nullptr;

    uint64_t key = glyphKey(fontSize, resolvedCodepoint);
    auto cached = glyphCache.find(key);
    if (cached != glyphCache.end()) {
        touchGlyph(key, cached->second);
        return &cached->second;
    }

    GlyphEntry glyph;
    int maxx = 0;
    int miny = 0;
    if (TTF_GlyphMetrics32(font, resolvedCodepoint, &glyph.minx, &maxx, &miny,
                           &glyph.maxy, &glyph.advance) < 0) {
        return nullptr;
    }

    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface* surface = TTF_RenderGlyph32_Blended(font, resolvedCodepoint, white);
    if (surface) {
        glyph.width = surface->w;
        glyph.height = surface->h;
        glyph.bytes = static_cast<std::size_t>(surface->w) *
                      static_cast<std::size_t>(surface->h) * 4U;
        if (surface->w > 0 && surface->h > 0) {
            glyph.texture = SDL_CreateTextureFromSurface(renderer, surface);
            if (glyph.texture)
                SDL_SetTextureBlendMode(glyph.texture, SDL_BLENDMODE_BLEND);
        }
        SDL_FreeSurface(surface);
    }

    glyphLru.push_front(key);
    glyph.lruIt = glyphLru.begin();
    auto inserted = glyphCache.emplace(key, std::move(glyph));
    glyphCacheBytes += inserted.first->second.bytes;
    evictGlyphs();

    auto it = glyphCache.find(key);
    return it != glyphCache.end() ? &it->second : nullptr;
}

void FontManager::drawText(SDL_Renderer* renderer, const char* text,
                           int x, int y, int fontSize, SDL_Color color) {
    if (!renderer || !text || !text[0])
        return;

    TTF_Font* font = getFont(fontSize);
    if (!font)
        return;

    int penX = x;
    Uint32 previousCodepoint = 0;
    bool hasPrevious = false;
    const char* p = text;
    while (*p) {
        Uint32 codepoint = 0;
        if (!decodeUtf8Codepoint(p, codepoint))
            break;

        if (codepoint == '\r' || codepoint == '\n')
            break;

        if (codepoint == '\t')
            codepoint = ' ';

        Uint32 resolvedCodepoint = resolveGlyphCodepoint(font, codepoint);
        if (resolvedCodepoint == 0)
            continue;

        if (hasPrevious && TTF_GetFontKerning(font)) {
            int kerning = TTF_GetFontKerningSizeGlyphs32(font, previousCodepoint,
                                                         resolvedCodepoint);
            if (kerning != 0)
                penX += kerning;
        }

        GlyphEntry* glyph = getGlyph(renderer, font, fontSize, codepoint);
        if (!glyph)
            continue;

        if (glyph->texture && glyph->width > 0 && glyph->height > 0) {
            SDL_SetTextureColorMod(glyph->texture, color.r, color.g, color.b);
            SDL_SetTextureAlphaMod(glyph->texture, color.a);

            SDL_Rect dst = {
                penX,
                y,
                glyph->width,
                glyph->height
            };
            SDL_RenderCopy(renderer, glyph->texture, nullptr, &dst);
        }

        penX += glyph->advance;
        previousCodepoint = resolvedCodepoint;
        hasPrevious = true;
    }
}

int FontManager::measureText(const char* text, int fontSize) {
    if (!text || !text[0])
        return 0;

    TTF_Font* font = getFont(fontSize);
    if (!font)
        return 0;

    int width = 0;
    Uint32 previousCodepoint = 0;
    bool hasPrevious = false;
    const char* p = text;
    while (*p) {
        Uint32 codepoint = 0;
        if (!decodeUtf8Codepoint(p, codepoint))
            break;

        if (codepoint == '\r' || codepoint == '\n')
            break;

        if (codepoint == '\t')
            codepoint = ' ';

        Uint32 resolvedCodepoint = resolveGlyphCodepoint(font, codepoint);
        if (resolvedCodepoint == 0)
            continue;

        if (hasPrevious && TTF_GetFontKerning(font)) {
            int kerning = TTF_GetFontKerningSizeGlyphs32(font, previousCodepoint,
                                                         resolvedCodepoint);
            if (kerning != 0)
                width += kerning;
        }

        auto cached = glyphCache.find(glyphKey(fontSize, resolvedCodepoint));
        if (cached != glyphCache.end()) {
            width += cached->second.advance;
            previousCodepoint = resolvedCodepoint;
            hasPrevious = true;
            continue;
        }

        int minx = 0;
        int maxx = 0;
        int miny = 0;
        int maxy = 0;
        int advance = 0;
        if (TTF_GlyphMetrics32(font, resolvedCodepoint, &minx, &maxx, &miny, &maxy,
                               &advance) == 0) {
            width += advance;
            previousCodepoint = resolvedCodepoint;
            hasPrevious = true;
        }
    }
    return width;
}

int FontManager::fontHeight(int fontSize) {
    TTF_Font* font = getFont(fontSize);
    if (!font) return fontSize;
    return TTF_FontHeight(font);
}

void FontManager::drawTextEllipsis(SDL_Renderer* renderer, const char* text,
                                   int x, int y, int fontSize, SDL_Color color,
                                   int maxWidth) {
    if (!text || !text[0] || maxWidth <= 0)
        return;

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
        popLastUtf8Codepoint(s);
        if (s.empty())
            break;
        if (measureText(s.c_str(), fontSize) + ellW <= maxWidth) {
            s += ell;
            drawText(renderer, s.c_str(), x, y, fontSize, color);
            return;
        }
    }
    drawText(renderer, ell, x, y, fontSize, color);
}

} // namespace xxplore
