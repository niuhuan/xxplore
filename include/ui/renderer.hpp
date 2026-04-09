#pragma once
#include <SDL.h>

namespace xxplore {

/// Wraps SDL2 window/renderer for 2D drawing primitives and texture management.
class Renderer {
public:
    /// Create SDL window + hardware-accelerated renderer (1280x720, vsync).
    bool init();
    /// Destroy renderer, window, and quit SDL + SDL_image.
    void shutdown();

    /// Fill the entire screen with a solid color.
    void clear(SDL_Color color);
    /// Flip the back-buffer to screen.
    void present();

    void drawRectFilled(int x, int y, int w, int h, SDL_Color color);
    void drawRect(int x, int y, int w, int h, SDL_Color color);
    void drawLine(int x1, int y1, int x2, int y2, SDL_Color color);

    /// Filled rounded rectangle (SDL2_gfx).
    void drawRoundedRectFilled(int x, int y, int w, int h, int radius, SDL_Color color);
    void drawRoundedRect(int x, int y, int w, int h, int radius, SDL_Color color);

    /// Restrict drawing to a rectangular region. Call clearClipRect() to undo.
    void setClipRect(int x, int y, int w, int h);
    void clearClipRect();

    /// Load a PNG/JPG image from filesystem path and return an SDL_Texture.
    /// Caller is responsible for calling SDL_DestroyTexture when done.
    SDL_Texture* loadTexture(const char* path);

    /// Draw an SDL_Texture at the given position and size.
    void drawTexture(SDL_Texture* tex, int x, int y, int w, int h);

    /// Create a blank RGBA texture usable as a render target.
    /// Caller owns the returned texture and must SDL_DestroyTexture it.
    SDL_Texture* createRenderTarget(int w, int h);
    /// Redirect all subsequent drawing to an off-screen texture.
    void setRenderTarget(SDL_Texture* tex);
    /// Restore drawing to the default screen back-buffer.
    void resetRenderTarget();

    SDL_Renderer* sdl() const { return sdlRenderer; }

private:
    SDL_Window*   sdlWindow   = nullptr;
    SDL_Renderer* sdlRenderer = nullptr;
    bool sdlInited = false;
};

} // namespace xxplore
