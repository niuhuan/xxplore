#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xplore {

class Renderer;
class FontManager;
class I18n;

struct InstallQueueItem {
    std::string path;
    std::string name;
    uint64_t    size = 0;
};

enum class InstallDeleteMode { KeepFiles, DeleteAfterInstall };
enum class InstallTarget { Nand, SdCard };
enum class InstallerAction { None, Close, ExitApp };

class InstallerScreen {
public:
    ~InstallerScreen() { joinWorker(); }

    void open(std::vector<InstallQueueItem> items, InstallDeleteMode deleteMode, bool appletMode);
    void close();

    bool isOpen() const { return open_; }
    bool isBusy() const { return open_ && (state_ == State::Loading || state_ == State::Running); }

    void update();
    InstallerAction handleInput(uint64_t kDown);
    void render(Renderer& renderer, FontManager& fm, const I18n& i18n) const;

private:
    enum class State { Closed, Loading, Ready, Running, Finished, Failed };

    void appendLog(std::string line);
    void joinWorker();
    void startInstallWorker();

    bool                     open_         = false;
    State                    state_        = State::Closed;
    std::vector<InstallQueueItem> items_;
    InstallDeleteMode        deleteMode_   = InstallDeleteMode::KeepFiles;
    bool                     appletMode_   = false;
    size_t                   loadingIndex_ = 0;
    uint64_t                 totalBytes_   = 0;
    int64_t                  nandFree_     = -1;
    int64_t                  sdFree_       = -1;
    InstallTarget            target_       = InstallTarget::SdCard;
    int                      focusRow_     = 1; // 0 target row, 1 button row
    int                      targetFocusCol_ = 1;
    int                      buttonFocusCol_ = 0;
    std::deque<std::string>  logs_;
    float                    currentProgress_ = 0.0f;
    float                    totalProgress_   = 0.0f;
    std::string              currentStatus_;
    std::string              errorMessage_;
    mutable std::mutex       mutex_;
    std::thread              worker_;
};

} // namespace xplore
