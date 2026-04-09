#include "util/screen_awake.hpp"

#include <switch.h>

#include <mutex>

namespace xxplore::util {

namespace {

std::mutex gScreenAwakeMutex;
int gScreenAwakeRefCount = 0;

} // namespace

void acquireScreenAwake() {
    std::lock_guard<std::mutex> lock(gScreenAwakeMutex);
    if (gScreenAwakeRefCount++ == 0)
        appletSetMediaPlaybackState(true);
}

void releaseScreenAwake() {
    std::lock_guard<std::mutex> lock(gScreenAwakeMutex);
    if (gScreenAwakeRefCount <= 0) {
        gScreenAwakeRefCount = 0;
        return;
    }
    if (--gScreenAwakeRefCount == 0)
        appletSetMediaPlaybackState(false);
}

} // namespace xxplore::util
