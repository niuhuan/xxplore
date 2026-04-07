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

/// Scrollable vertical list of ListItems.
/// Each instance maintains its own cursor position and scroll offset.
class FileList {
public:
    /// Replace all items and reset cursor / scroll to the top.
    void setItems(std::vector<ListItem> newItems);

    const std::vector<ListItem>& getItems() const { return items; }

    void moveCursorUp();
    void moveCursorDown();
    int  getCursor() const { return cursor; }

    /// Return the item under the cursor, or nullptr if list is empty.
    const ListItem* getSelectedItem() const;

    /// Draw the list within the given rectangle.
    /// @param active  if true, the cursor highlight is visible.
    void render(Renderer& renderer, FontManager& fontManager,
                int x, int y, int width, int height, bool active);

private:
    std::vector<ListItem> items;
    int cursor    = 0;
    int scrollTop = 0;
};

} // namespace xplore
