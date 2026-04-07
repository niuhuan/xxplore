#include "ui/file_list.hpp"
#include "ui/renderer.hpp"
#include "ui/font_manager.hpp"
#include "ui/theme.hpp"

namespace xplore {

FileList::~FileList() {
    destroyCache();
}

void FileList::setItems(std::vector<ListItem> newItems, bool hasGoUpEntry,
                        bool allowSelection) {
    items = std::move(newItems);
    cursor = 0;
    scrollTop = 0;
    selection.clear();
    selectionEnabled     = allowSelection;
    firstSelectableIndex = hasGoUpEntry ? 1 : 0;
    dirty = true;
}

void FileList::reloadItems(std::vector<ListItem> newItems, bool hasGoUpEntry,
                           bool allowSelection, int preserveCursorIndex) {
    items = std::move(newItems);
    selection.clear();
    selectionEnabled     = allowSelection;
    firstSelectableIndex = hasGoUpEntry ? 1 : 0;
    int n                = static_cast<int>(items.size());
    if (n <= 0)
        cursor = 0;
    else if (preserveCursorIndex >= n)
        cursor = n - 1;
    else if (preserveCursorIndex < 0)
        cursor = 0;
    else
        cursor = preserveCursorIndex;
    scrollTop = 0;
    dirty     = true;
}

// --- cursor movement ---

void FileList::moveCursorUp() {
    if (cursor > 0) {
        cursor--;
        dirty = true;
    }
}

void FileList::moveCursorDown() {
    if (cursor < (int)items.size() - 1) {
        cursor++;
        dirty = true;
    }
}

void FileList::moveCursorPageUp(int pageItems) {
    if (items.empty()) return;
    cursor -= pageItems;
    if (cursor < 0) cursor = 0;
    dirty = true;
}

void FileList::moveCursorPageDown(int pageItems) {
    if (items.empty()) return;
    cursor += pageItems;
    int last = (int)items.size() - 1;
    if (cursor > last) cursor = last;
    dirty = true;
}

const ListItem* FileList::getSelectedItem() const {
    if (cursor >= 0 && cursor < (int)items.size())
        return &items[cursor];
    return nullptr;
}

// --- multi-selection ---

void FileList::toggleSelect() {
    if (items.empty() || !selectionEnabled) return;

    if (cursor >= firstSelectableIndex) {
        if (selection.count(cursor))
            selection.erase(cursor);
        else
            selection.insert(cursor);
    }

    cursor++;
    if (cursor >= (int)items.size())
        cursor = 0;
    dirty = true;
}

void FileList::toggleSelectAll() {
    if (!selectionEnabled) return;
    if (allSelected()) {
        selection.clear();
    } else {
        selection.clear();
        for (int i = firstSelectableIndex; i < (int)items.size(); i++)
            selection.insert(i);
    }
    dirty = true;
}

bool FileList::allSelected() const {
    int selectable = (int)items.size() - firstSelectableIndex;
    return selectable > 0 && (int)selection.size() == selectable;
}

void FileList::clearSelection() {
    if (!selection.empty()) {
        selection.clear();
        dirty = true;
    }
}

// --- render cache ---

void FileList::updateCache(Renderer& renderer, FontManager& fontManager,
                           int width, int height) {
    bool sizeChanged = (width != cacheW || height != cacheH);

    if (sizeChanged) {
        destroyCache();
        cacheW = width;
        cacheH = height;
        cachedTex = renderer.createRenderTarget(cacheW, cacheH);
        dirty = true;
    }

    if (!dirty || !cachedTex)
        return;

    dirty = false;

    renderer.setRenderTarget(cachedTex);
    renderer.clear(theme::BG);

    // Auto-scroll to keep cursor visible
    int cursorTop = cursor * theme::ITEM_H;
    int cursorBot = cursorTop + theme::ITEM_H;
    if (cursorTop < scrollTop)
        scrollTop = cursorTop;
    else if (cursorBot > scrollTop + height)
        scrollTop = cursorBot - height;

    bool selMode = selectionEnabled && hasSelection();
    int checkboxSpace = selMode ? (theme::CHECKBOX_SIZE + theme::PADDING_SM) : 0;
    int iconX = theme::PADDING + checkboxSpace;
    int textX = iconX + theme::ICON_SIZE + theme::PADDING_SM;

    for (int i = 0; i < (int)items.size(); i++) {
        int iy = i * theme::ITEM_H - scrollTop;

        if (iy + theme::ITEM_H < 0 || iy > height)
            continue;

        // Selection highlight (behind cursor)
        if (isSelected(i)) {
            renderer.drawRectFilled(0, iy, width, theme::ITEM_H,
                                    theme::SELECTED_ROW);
            renderer.drawRectFilled(0, iy, 3, theme::ITEM_H,
                                    theme::SELECTED_BAR);
        }

        // Cursor highlight
        if (i == cursor) {
            renderer.drawRectFilled(0, iy, width, theme::ITEM_H,
                                    theme::CURSOR_ROW);
        }

        // Checkbox (selection mode only, for selectable items)
        if (selMode && i >= firstSelectableIndex) {
            int cbX = theme::PADDING;
            int cbY = iy + (theme::ITEM_H - theme::CHECKBOX_SIZE) / 2;
            if (isSelected(i)) {
                renderer.drawRectFilled(cbX, cbY,
                    theme::CHECKBOX_SIZE, theme::CHECKBOX_SIZE,
                    theme::CHECKBOX_FILL);
                // Check mark (✓) in white, 2px thick
                SDL_Color white = theme::ON_PRIMARY;
                renderer.drawLine(cbX+5,  cbY+12, cbX+10, cbY+17, white);
                renderer.drawLine(cbX+6,  cbY+12, cbX+11, cbY+17, white);
                renderer.drawLine(cbX+10, cbY+17, cbX+19, cbY+7,  white);
                renderer.drawLine(cbX+11, cbY+17, cbX+20, cbY+7,  white);
            } else {
                renderer.drawRect(cbX, cbY,
                    theme::CHECKBOX_SIZE, theme::CHECKBOX_SIZE,
                    theme::CHECKBOX_BORDER);
            }
        }

        // Icon
        if (items[i].icon) {
            int iconY = iy + (theme::ITEM_H - theme::ICON_SIZE) / 2;
            renderer.drawTexture(items[i].icon, iconX, iconY,
                                 theme::ICON_SIZE, theme::ICON_SIZE);
        }

        // Label
        fontManager.drawText(renderer.sdl(), items[i].label.c_str(),
            textX,
            iy + (theme::ITEM_H - theme::FONT_SIZE_ITEM) / 2,
            theme::FONT_SIZE_ITEM, theme::TEXT);

        // Divider
        renderer.drawRectFilled(
            theme::PADDING, iy + theme::ITEM_H - 1,
            width - theme::PADDING * 2, 1,
            theme::DIVIDER);
    }

    renderer.resetRenderTarget();
}

void FileList::destroyCache() {
    if (cachedTex) {
        SDL_DestroyTexture(cachedTex);
        cachedTex = nullptr;
    }
    cacheW = 0;
    cacheH = 0;
}

} // namespace xplore
