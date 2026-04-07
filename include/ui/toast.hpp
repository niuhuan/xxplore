#pragma once
#include <SDL.h>
#include <string>

namespace xplore {

enum class ToastKind {
    Error,
    Success,
    Warning,
    Info,
};

class Renderer;
class FontManager;

/// Timed notification bar shown at the bottom of the screen.
/// Displays a title line (e.g. operation context) and a detail line (error message).
/// Auto-hides after a configurable duration (default 3.2 s).
class Toast {
public:
    /// Show the toast with a title and detail message.
    void show(const std::string& title, const std::string& detail,
              ToastKind kind = ToastKind::Error, uint32_t durationMs = 3200);
    /// Advance the internal timer by deltaMs. Toast disappears when timer reaches 0.
    void update(uint32_t deltaMs);
    /// Draw the toast overlay (no-op when not visible).
    void render(Renderer& renderer, FontManager& fontManager);
    bool isVisible() const { return remainingMs > 0; }

private:
    std::string title;
    std::string detail;
    ToastKind kind = ToastKind::Error;
    uint32_t remainingMs = 0;
};

} // namespace xplore
