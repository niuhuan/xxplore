#pragma once
#include <SDL.h>
#include <string>
#include <vector>

namespace xplore {

class Renderer;
class FontManager;

/// A single item in a FileList. Holds display label, optional icon texture,
/// and an opaque action id for the caller to interpret on selection.
struct ListItem {
    std::string  label;
    SDL_Texture* icon   = nullptr;  ///< Non-owning; texture lifetime managed elsewhere
    int          action = 0;
};

/// Scrollable vertical list of ListItems with off-screen render caching.
/// Each instance maintains its own cursor, scroll offset, and a cached
/// render-target texture that is only re-drawn when the dirty flag is set.
class FileList {
public:
    ~FileList();

    /// Replace all items and reset cursor / scroll to the top. Marks dirty.
    void setItems(std::vector<ListItem> newItems);

    const std::vector<ListItem>& getItems() const { return items; }

    /// Move cursor up/down. Marks dirty.
    void moveCursorUp();
    void moveCursorDown();
    int  getCursor() const { return cursor; }

    /// Return the item under the cursor, or nullptr if list is empty.
    const ListItem* getSelectedItem() const;

    /// Force the cached texture to be re-drawn on the next updateCache() call.
    void markDirty() { dirty = true; }

    /// Re-render the list content into the internal render-target texture.
    /// Only performs work when the dirty flag is set or the cache size changed.
    /// Renders at (0,0) origin; caller positions the texture during compositing.
    void updateCache(Renderer& renderer, FontManager& fontManager,
                     int width, int height);

    /// Return the cached off-screen texture for compositing onto the screen.
    SDL_Texture* getCachedTexture() const { return cachedTex; }

    /// Release the cached render-target texture (e.g. before shutdown).
    void destroyCache();

private:
    std::vector<ListItem> items;
    int cursor    = 0;
    int scrollTop = 0;

    SDL_Texture* cachedTex = nullptr;
    bool dirty   = true;
    int  cacheW  = 0;
    int  cacheH  = 0;
};

} // namespace xplore
