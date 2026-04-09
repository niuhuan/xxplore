#include "ui/image_viewer.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include <SDL_image.h>
#include <switch.h>

namespace xxplore {

ImageViewer::~ImageViewer() {
    close();
}

bool ImageViewer::open(Renderer& renderer, const std::string& path, std::string& errOut) {
    close();

    SDL_Surface* surf = IMG_Load(path.c_str());
    if (!surf && path.size() >= 4 && path.substr(path.size() - 4) == ".bmp")
        surf = SDL_LoadBMP(path.c_str());
    if (!surf) {
        errOut = IMG_GetError();
        if (errOut.empty())
            errOut = SDL_GetError();
        if (errOut.empty())
            errOut = "load image failed";
        return false;
    }

    texture = SDL_CreateTextureFromSurface(renderer.sdl(), surf);
    texW    = surf->w;
    texH    = surf->h;
    SDL_FreeSurface(surf);

    if (!texture) {
        errOut = SDL_GetError();
        if (errOut.empty())
            errOut = "create image texture failed";
        close();
        return false;
    }

    active = true;
    return true;
}

void ImageViewer::close() {
    active = false;
    texW   = 0;
    texH   = 0;
    if (texture) {
        SDL_DestroyTexture(texture);
        texture = nullptr;
    }
}

bool ImageViewer::handleInput(uint64_t kDown) {
    if (!active)
        return false;
    if (kDown & (HidNpadButton_B | HidNpadButton_Plus)) {
        close();
        return true;
    }
    return false;
}

void ImageViewer::render(Renderer& renderer) const {
    if (!active || !texture || texW <= 0 || texH <= 0)
        return;

    renderer.drawRectFilled(0, 0, theme::SCREEN_W, theme::SCREEN_H, theme::MENU_OVERLAY);

    float scaleX = static_cast<float>(theme::SCREEN_W) / static_cast<float>(texW);
    float scaleY = static_cast<float>(theme::SCREEN_H) / static_cast<float>(texH);
    float scale  = scaleX < scaleY ? scaleX : scaleY;
    if (scale > 1.0f)
        scale = 1.0f;

    int drawW = static_cast<int>(static_cast<float>(texW) * scale);
    int drawH = static_cast<int>(static_cast<float>(texH) * scale);
    int drawX = (theme::SCREEN_W - drawW) / 2;
    int drawY = (theme::SCREEN_H - drawH) / 2;

    renderer.drawTexture(texture, drawX, drawY, drawW, drawH);
    renderer.drawRect(drawX - 1, drawY - 1, drawW + 2, drawH + 2, theme::DIVIDER);
}

} // namespace xxplore
