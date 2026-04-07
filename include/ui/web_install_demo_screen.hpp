#pragma once
#include "util/web_install_demo_server.hpp"
#include <cstdint>

namespace xplore {

class Renderer;
class FontManager;

enum class WebInstallDemoAction { None, Close };

class WebInstallDemoScreen {
public:
    ~WebInstallDemoScreen() { close(); }

    void open();
    void close();

    bool isOpen() const { return open_; }
    WebInstallDemoAction handleInput(uint64_t kDown);
    void render(Renderer& renderer, FontManager& fm) const;

private:
    bool open_ = false;
    WebInstallDemoServer server_;
};

} // namespace xplore
