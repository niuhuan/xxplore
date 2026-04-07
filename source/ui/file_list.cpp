#include "ui/file_list.hpp"
#include "ui/renderer.hpp"
#include "ui/font_manager.hpp"
#include "ui/theme.hpp"

namespace xplore {

FileList::~FileList() {
    destroyCache();
}

void FileList::setItems(std::vector<ListItem> newItems) {
    items = std::move(newItems);
    cursor = 0;
    scrollTop = 0;
    dirty = true;
}

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

const ListItem* FileList::getSelectedItem() const {
    if (cursor >= 0 && cursor < (int)items.size())
        return &items[cursor];
    return nullptr;
}

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

    // Clear the render target with background color
    renderer.clear(theme::BG);

    // Auto-scroll to keep cursor visible
    int cursorTop = cursor * theme::ITEM_H;
    int cursorBot = cursorTop + theme::ITEM_H;
    if (cursorTop < scrollTop)
        scrollTop = cursorTop;
    else if (cursorBot > scrollTop + height)
        scrollTop = cursorBot - height;

    int iconX = theme::PADDING;
    int textX = iconX + theme::ICON_SIZE + theme::PADDING_SM;

    for (int i = 0; i < (int)items.size(); i++) {
        int iy = i * theme::ITEM_H - scrollTop;

        if (iy + theme::ITEM_H < 0 || iy > height)
            continue;

        // Always render cursor highlight; inactive mask hides it during compositing
        if (i == cursor) {
            renderer.drawRectFilled(0, iy, width, theme::ITEM_H,
                                    theme::CURSOR_ROW);
        }

        if (items[i].icon) {
            int iconY = iy + (theme::ITEM_H - theme::ICON_SIZE) / 2;
            renderer.drawTexture(items[i].icon, iconX, iconY,
                                 theme::ICON_SIZE, theme::ICON_SIZE);
        }

        fontManager.drawText(renderer.sdl(), items[i].label.c_str(),
            textX,
            iy + (theme::ITEM_H - theme::FONT_SIZE_ITEM) / 2,
            theme::FONT_SIZE_ITEM, theme::TEXT);

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
