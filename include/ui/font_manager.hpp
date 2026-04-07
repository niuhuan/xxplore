#pragma once
#include <SDL.h>
#include <SDL_ttf.h>
#include <string>
#include <unordered_map>

namespace xplore {

/// Manages TTF font loading and UTF-8 text rendering via SDL2_ttf.
/// Fonts are opened from a single .ttf file; different point sizes are cached.
class FontManager {
public:
    /// Initialize SDL_ttf. Call after Renderer::init() and romfsInit().
    /// @param fontPath  path to the .ttf file (e.g. "romfs:/fonts/xplore.ttf")
    bool init(const char* fontPath);
    /// Close all cached fonts and shut down SDL_ttf.
    void shutdown();

    /// Render a UTF-8 string. Text is blended (anti-aliased) onto the renderer.
    void drawText(SDL_Renderer* renderer, const char* text,
                  int x, int y, int fontSize, SDL_Color color);
    /// Return the pixel width of a UTF-8 string without drawing it.
    int measureText(const char* text, int fontSize);
    /// Return the line height for a given point size.
    int fontHeight(int fontSize);

    /// Draw UTF-8 text truncated with "..." if wider than @p maxWidth.
    void drawTextEllipsis(SDL_Renderer* renderer, const char* text,
                          int x, int y, int fontSize, SDL_Color color, int maxWidth);

private:
    /// Open (or return cached) TTF_Font for the requested point size.
    TTF_Font* getFont(int size);

    std::string fontPath;
    std::unordered_map<int, TTF_Font*> fontCache;
    bool ttfInited = false;
};

} // namespace xplore
