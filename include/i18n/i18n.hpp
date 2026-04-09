#pragma once
#include <string>
#include <unordered_map>

namespace xxplore {

/// Lightweight key=value i18n loader.
/// Translation files are plain text with `key=value` lines (# comments, UTF-8).
/// Files live in romfs:/i18n/<lang>.ini and are loaded at startup.
class I18n {
public:
    /// Load translations from an .ini file. Returns false on I/O error.
    bool load(const char* path);

    /// Look up a translated string by key. Returns the key itself if missing.
    const char* t(const char* key) const;

private:
    std::unordered_map<std::string, std::string> table;
};

} // namespace xxplore
