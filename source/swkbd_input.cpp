#include "swkbd_input.hpp"
#include <cstring>
#include <switch.h>
#include <switch/applets/swkbd.h>

namespace xplore {

bool swkbdTextInput(const char* headerText, const char* guideText, const char* initialText,
                    char* out, size_t outLen) {
    if (!out || outLen < 2) return false;
    out[0] = '\0';

    SwkbdConfig kbd;
    Result rc = swkbdCreate(&kbd, 0);
    if (R_FAILED(rc)) return false;

    swkbdConfigMakePresetDefault(&kbd);
    if (headerText && headerText[0])
        swkbdConfigSetHeaderText(&kbd, headerText);
    if (guideText && guideText[0])
        swkbdConfigSetGuideText(&kbd, guideText);
    if (initialText && initialText[0])
        swkbdConfigSetInitialText(&kbd, initialText);

    rc = swkbdShow(&kbd, out, outLen);
    swkbdClose(&kbd);
    return R_SUCCEEDED(rc);
}

} // namespace xplore
