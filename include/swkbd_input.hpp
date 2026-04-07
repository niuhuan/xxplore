#pragma once
#include <cstddef>

namespace xplore {

/// Blocking software keyboard. On success fills @p out (NUL-terminated). Returns false on cancel/error.
bool swkbdTextInput(const char* headerText, const char* guideText, const char* initialText,
                    char* out, size_t outLen);

} // namespace xplore
