#pragma once
#include <SDL.h>
#include <string>

namespace xplore {

class Renderer;
class FontManager;

class Toast {
public:
    void show(const std::string& title, const std::string& detail,
              uint32_t durationMs = 3200);
    void update(uint32_t deltaMs);
    void render(Renderer& renderer, FontManager& fontManager);
    bool isVisible() const { return remainingMs > 0; }

private:
    std::string title;
    std::string detail;
    uint32_t remainingMs = 0;
};

} // namespace xplore
