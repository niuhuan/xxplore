#pragma once
#include <string>
#include <vector>

namespace xplore {

class Renderer;
class FontManager;

struct ListItem {
    std::string label;
    int action = 0;
};

class FileList {
public:
    void setItems(std::vector<ListItem> newItems);
    const std::vector<ListItem>& getItems() const { return items; }

    void moveCursorUp();
    void moveCursorDown();
    int  getCursor() const { return cursor; }
    const ListItem* getSelectedItem() const;

    void render(Renderer& renderer, FontManager& fontManager,
                int x, int y, int width, int height, bool active);

private:
    std::vector<ListItem> items;
    int cursor    = 0;
    int scrollTop = 0;
};

} // namespace xplore
