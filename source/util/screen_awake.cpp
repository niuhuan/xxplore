#include "util/screen_awake.hpp"

#include <switch.h>

#include <mutex>

namespace xxplore::util {

namespace {

std::mutex gScreenAwakeMutex;
int gScreenAwakeRefCount = 0;

// CPU boost (ApmCpuBoostMode_FastLoad) is available since HOS 7.0.0.
// It bumps CPU from 1020 MHz to 1785 MHz and forces GPU to ~76.8 MHz,
// which is desirable for IO/decompression heavy work (install, copy, zip).
// In applet mode the AM service may refuse the call; we silently ignore
// any failure so this remains best-effort.
void applyCpuBoost(bool enable) {
    if (!hosversionAtLeast(7, 0, 0))
        return;
    appletSetCpuBoostMode(enable ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}

} // namespace

void acquireScreenAwake() {
    std::lock_guard<std::mutex> lock(gScreenAwakeMutex);
    if (gScreenAwakeRefCount++ == 0) {
        appletSetMediaPlaybackState(true);
        applyCpuBoost(true);
    }
}

void releaseScreenAwake() {
    std::lock_guard<std::mutex> lock(gScreenAwakeMutex);
    if (gScreenAwakeRefCount <= 0) {
        gScreenAwakeRefCount = 0;
        return;
    }
    if (--gScreenAwakeRefCount == 0) {
        appletSetMediaPlaybackState(false);
        applyCpuBoost(false);
    }
}

} // namespace xxplore::util
