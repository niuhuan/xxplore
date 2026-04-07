#include "ui/file_list.hpp"
#include "ui/renderer.hpp"
#include "ui/font_manager.hpp"
#include "ui/theme.hpp"

namespace xplore {

void FileList::setItems(std::vector<ListItem> newItems) {
    items = std::move(newItems);
    cursor = 0;
    scrollTop = 0;
}

void FileList::moveCursorUp() {
    if (cursor > 0) cursor--;
}

void FileList::moveCursorDown() {
    if (cursor < (int)items.size() - 1) cursor++;
}

const ListItem* FileList::getSelectedItem() const {
    if (cursor >= 0 && cursor < (int)items.size())
        return &items[cursor];
    return nullptr;
}

void FileList::render(Renderer& renderer, FontManager& fontManager,
                      int x, int y, int width, int height, bool active) {
    renderer.setClipRect(x, y, width, height);
    renderer.drawRectFilled(x, y, width, height, theme::BG);

    // Auto-scroll to keep cursor visible
    int cursorTop = cursor * theme::ITEM_H;
    int cursorBot = cursorTop + theme::ITEM_H;
    if (cursorTop < scrollTop)
        scrollTop = cursorTop;
    else if (cursorBot > scrollTop + height)
        scrollTop = cursorBot - height;

    // Icon occupies ICON_SIZE width; text starts after icon slot + padding
    int iconX   = x + theme::PADDING;
    int textX   = iconX + theme::ICON_SIZE + theme::PADDING_SM;

    for (int i = 0; i < (int)items.size(); i++) {
        int iy = y + i * theme::ITEM_H - scrollTop;

        // Skip items outside the visible area
        if (iy + theme::ITEM_H < y || iy > y + height)
            continue;

        // Cursor highlight
        if (active && i == cursor) {
            renderer.drawRectFilled(x, iy, width, theme::ITEM_H,
                                    theme::CURSOR_ROW);
        }

        // Icon (centered vertically within the row)
        if (items[i].icon) {
            int iconY = iy + (theme::ITEM_H - theme::ICON_SIZE) / 2;
            renderer.drawTexture(items[i].icon, iconX, iconY,
                                 theme::ICON_SIZE, theme::ICON_SIZE);
        }

        // Label (vertically centered)
        fontManager.drawText(renderer.sdl(), items[i].label.c_str(),
            textX,
            iy + (theme::ITEM_H - theme::FONT_SIZE_ITEM) / 2,
            theme::FONT_SIZE_ITEM, theme::TEXT);

        // Bottom divider
        renderer.drawRectFilled(
            x + theme::PADDING, iy + theme::ITEM_H - 1,
            width - theme::PADDING * 2, 1,
            theme::DIVIDER);
    }

    renderer.clearClipRect();
}

} // namespace xplore
