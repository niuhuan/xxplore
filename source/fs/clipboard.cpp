#include "fs/clipboard.hpp"

namespace xxplore {
namespace fs {

namespace {

std::string normPathTrim(const std::string& s) {
    std::string p = s;
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();
    return p;
}

/// True if @p path is the same as or a descendant of @p ancestorDir (both normalized).
bool isSameOrUnder(const std::string& ancestor, const std::string& path) {
    std::string a = normPathTrim(ancestor);
    std::string p = normPathTrim(path);
    if (p == a) return true;
    if (p.size() > a.size() && p.compare(0, a.size(), a) == 0 && p[a.size()] == '/')
        return true;
    return false;
}

} // namespace

void Clipboard::set(std::string dir, std::vector<ClipboardEntry> ent, ClipboardOp o) {
    sourceDir = std::move(dir);
    entries   = std::move(ent);
    op        = o;
}

void Clipboard::clear() {
    sourceDir.clear();
    entries.clear();
    op = ClipboardOp::Copy;
}

bool clipboardPasteDestinationAllowed(const Clipboard& clip, const std::string& destDir) {
    if (clip.empty()) return false;
    std::string d = normPathTrim(destDir);
    // Copy or cut: cannot paste back into the same folder (duplicate names / no-op).
    if (d == normPathTrim(clip.sourceDirectory()))
        return false;
    for (const auto& e : clip.items()) {
        if (!e.isDirectory) continue;
        if (isSameOrUnder(e.fullPath, d))
            return false;
    }
    return true;
}

} // namespace fs
} // namespace xxplore
