#include "i18n/i18n.hpp"
#include <cstdio>
#include <cstring>

namespace xxplore {

bool I18n::load(const char* path) {
    table.clear();
    FILE* f = fopen(path, "r");
    if (!f) {
        printf("I18n: cannot open %s\n", path);
        return false;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0 || line[0] == '#')
            continue;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        table[std::string(line)] = std::string(eq + 1);
    }
    fclose(f);
    printf("I18n: loaded %zu keys from %s\n", table.size(), path);
    return true;
}

const char* I18n::t(const char* key) const {
    auto it = table.find(key);
    if (it != table.end())
        return it->second.c_str();
    return key;
}

} // namespace xxplore
