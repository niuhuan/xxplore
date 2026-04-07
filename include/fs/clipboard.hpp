#pragma once
#include <string>
#include <vector>

namespace xplore {
namespace fs {

enum class ClipboardOp { Copy, Cut };

struct ClipboardEntry {
    std::string name;
    bool        isDirectory = false;
    std::string fullPath;
};

/// Single-source-directory clipboard; new copy/cut replaces contents entirely.
class Clipboard {
public:
    void set(std::string sourceDir, std::vector<ClipboardEntry> entries, ClipboardOp op);
    void clear();

    bool empty() const { return entries.empty(); }
    ClipboardOp operation() const { return op; }
    const std::string& sourceDirectory() const { return sourceDir; }
    const std::vector<ClipboardEntry>& items() const { return entries; }

private:
    std::string                sourceDir;
    std::vector<ClipboardEntry> entries;
    ClipboardOp                op = ClipboardOp::Copy;
};

/// False when: cut onto same folder as source, or @p destDir is the same path as / under any
/// directory entry in the clipboard (cannot paste into self or own subfolder).
bool clipboardPasteDestinationAllowed(const Clipboard& clip, const std::string& destDir);

} // namespace fs
} // namespace xplore
