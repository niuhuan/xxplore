#pragma once
#include <SDL.h>
#include <string>

namespace xxplore {

class Renderer;

class ImageViewer {
public:
    ~ImageViewer();

    bool open(Renderer& renderer, const std::string& path, std::string& errOut);
    void close();

    bool isOpen() const { return active; }
    bool handleInput(uint64_t kDown);
    void render(Renderer& renderer) const;

private:
    SDL_Texture* texture = nullptr;
    bool         active  = false;
    int          texW    = 0;
    int          texH    = 0;
};

} // namespace xxplore
