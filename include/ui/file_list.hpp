#pragma once
#include <SDL.h>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace xxplore {

class Renderer;
class FontManager;

/// A single item in a FileList. Holds display label, optional icon texture,
/// and an opaque action id for the caller to interpret on selection.
struct ListItem {
    std::string  label;
    SDL_Texture* icon   = nullptr;  ///< Non-owning; texture lifetime managed elsewhere
    int          action = 0;
    uint64_t     size   = 0;
    std::string  metadata;          ///< Hidden payload (e.g. network drive id)
};

/// Scrollable vertical list of ListItems with off-screen render caching,
/// cursor navigation, page flip, and multi-selection support.
class FileList {
public:
    ~FileList();

    /// Replace all items and reset cursor / scroll / selection.
    /// @param hasGoUpEntry     true when the first item is ".." (non-selectable)
    /// @param selectionEnabled when false, Y/X selection is disabled (e.g. virtual root)
    void setItems(std::vector<ListItem> newItems, bool hasGoUpEntry = false,
                  bool selectionEnabled = true);

    /// Replace items, clear selection, keep cursor index clamped to new size.
    void reloadItems(std::vector<ListItem> newItems, bool hasGoUpEntry,
                     bool selectionEnabled, int preserveCursorIndex);

    const std::vector<ListItem>& getItems() const { return items; }

    // --- cursor movement ---
    void moveCursorUp();
    void moveCursorDown();
    void moveCursorPageUp(int pageItems);
    void moveCursorPageDown(int pageItems);
    void setCursor(int index);
    int  hitTestIndex(int localY) const;
    int  getCursor() const { return cursor; }

    /// Return the item under the cursor, or nullptr if list is empty.
    const ListItem* getSelectedItem() const;

    // --- multi-selection ---
    /// Toggle current item's selection, then advance cursor (wrapping to 0).
    void toggleSelect();
    /// Select all / deselect all (excluding ".." entry).
    void toggleSelectAll();
    bool isSelected(int index) const { return selection.count(index) > 0; }
    int  selectionCount() const { return static_cast<int>(selection.size()); }
    bool hasSelection() const { return !selection.empty(); }
    bool allSelected() const;
    void clearSelection();

    // --- render cache ---
    void markDirty() { dirty = true; }
    void updateCache(Renderer& renderer, FontManager& fontManager,
                     int width, int height);
    SDL_Texture* getCachedTexture() const { return cachedTex; }
    void destroyCache();

private:
    std::vector<ListItem> items;
    int cursor    = 0;
    int scrollTop = 0;

    int  firstSelectableIndex = 0;
    bool selectionEnabled     = true;
    std::set<int> selection;

    SDL_Texture* cachedTex = nullptr;
    bool dirty   = true;
    int  cacheW  = 0;
    int  cacheH  = 0;
};

} // namespace xxplore
