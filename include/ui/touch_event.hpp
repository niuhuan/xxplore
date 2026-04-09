#pragma once

namespace xxplore {

struct TouchTap {
    bool active = false;
    int x = 0;
    int y = 0;
};

inline bool pointInRect(const TouchTap* tap, int x, int y, int w, int h) {
    return tap && tap->active && tap->x >= x && tap->x < (x + w) &&
           tap->y >= y && tap->y < (y + h);
}

} // namespace xxplore
