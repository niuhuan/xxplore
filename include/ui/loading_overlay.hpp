#pragma once
#include <cstdint>
#include <string>

namespace xplore {

class Renderer;
class FontManager;
class I18n;

/// Full-screen loading overlay shown during network operations.
/// Shows after a short delay to avoid flicker on fast ops.
/// Has a configurable timeout; fires onTimeout when exceeded.
class LoadingOverlay {
public:
    /// Start the overlay. It becomes visible after delayMs.
    void show(std::string message, uint32_t timeoutMs = 15000, uint32_t delayMs = 400);

    /// Cancel / hide the overlay.
    void hide();

    bool isActive() const { return active_; }
    bool isVisible() const { return active_ && visible_; }
    bool hasTimedOut() const { return timedOut_; }

    /// Call every frame with delta milliseconds.
    void update(uint32_t deltaMs);

    /// Render the overlay (only if visible after delay).
    void render(Renderer& renderer, FontManager& fm, const I18n& i18n);

private:
    bool        active_    = false;
    bool        visible_   = false;  // becomes true after delay
    bool        timedOut_  = false;
    std::string message_;
    uint32_t    delayMs_   = 400;
    uint32_t    timeoutMs_ = 15000;
    uint32_t    elapsed_   = 0;
    uint32_t    dotTimer_  = 0;
    int         dotCount_  = 0;
};

} // namespace xplore
