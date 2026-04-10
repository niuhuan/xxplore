#include "ui/installer_screen.hpp"
#include "fs/fs_api.hpp"
#include "install/backend.hpp"
#include "i18n/i18n.hpp"
#include "ui/font_manager.hpp"
#include "ui/panel_chrome.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "util/screen_awake.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <switch.h>

namespace xxplore {

namespace {

constexpr int kTitleActionButtonW = 132;
constexpr SDL_Color kAppletWarningYellow = {0xf5, 0x9e, 0x0b, 0xff};

bool hasCompressedInstallPackage(const std::vector<InstallQueueItem>& items) {
    for (const auto& item : items) {
        std::size_t dot = item.path.find_last_of('.');
        if (dot == std::string::npos)
            continue;
        std::string ext = item.path.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (ext == ".nsz" || ext == ".xcz")
            return true;
    }
    return false;
}

void drawProgressBar(Renderer& renderer, int x, int y, int w, int h, float progress) {
    if (progress < 0.0f)
        progress = 0.0f;
    if (progress > 1.0f)
        progress = 1.0f;

    renderer.drawRoundedRectFilled(x, y, w, h, 8, theme::SURFACE);
    int fillW = static_cast<int>(static_cast<float>(w) * progress);
    if (progress > 0.0f && fillW <= 0)
        fillW = 1;
    if (fillW > w)
        fillW = w;
    if (fillW > 0)
        renderer.drawRoundedRectFilled(x, y, fillW, h, 8, theme::PRIMARY);
    renderer.drawRoundedRect(x, y, w, h, 8, theme::DIVIDER);
}

SDL_Color withAlpha(SDL_Color color, Uint8 alpha) {
    color.a = alpha;
    return color;
}

std::string formatInstallSpeed(uint64_t bytesPerSec) {
    char buf[64];
    if (bytesPerSec >= 1024ULL * 1024ULL) {
        std::snprintf(buf, sizeof(buf), "%.1f MiB/s",
                      static_cast<double>(bytesPerSec) / (1024.0 * 1024.0));
    } else if (bytesPerSec >= 1024ULL) {
        std::snprintf(buf, sizeof(buf), "%.1f KiB/s",
                      static_cast<double>(bytesPerSec) / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%llu B/s",
                      static_cast<unsigned long long>(bytesPerSec));
    }
    return buf;
}

} // namespace

void InstallerScreen::open(std::vector<InstallQueueItem> items, InstallDeleteMode deleteMode,
                           bool appletMode, const I18n& i18n,
                           std::shared_ptr<InstallDataSourceCallbacks> sourceCallbacks) {
    joinWorker();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        open_            = true;
        state_           = State::Loading;
        items_           = std::move(items);
        deleteMode_      = deleteMode;
        appletMode_      = appletMode;
        showCompressedAppletWarning_ = appletMode && hasCompressedInstallPackage(items);
        loadingIndex_    = 0;
        totalBytes_      = 0;
        nandFree_        = -1;
        sdFree_          = -1;
        target_          = InstallTarget::SdCard;
        focusRow_        = 1;
        targetFocusCol_  = 1;
        buttonFocusCol_  = 0;
        logs_.clear();
        currentProgress_ = 0.0f;
        totalProgress_   = 0.0f;
        currentStatus_.clear();
        errorMessage_.clear();
        interruptButtonLabel_ = std::string(i18n.t("installer.abort_button")) + " (-)";
        interruptConfirmTitle_ = i18n.t("installer.abort_confirm_title");
        interruptConfirmBody_ = i18n.t("installer.abort_confirm_body");
        abortedByUser_    = false;
        abortRequested_.store(false);
        interruptConfirm_.close();
        speedMeter_.reset();
        sourceCallbacks_ = std::move(sourceCallbacks);
    }
    appendLog(appletMode ? "Applet mode detected." : "Title override mode detected.");
    appendLog("Preparing install queue...");
}

void InstallerScreen::close() {
    joinWorker();
    std::lock_guard<std::mutex> lock(mutex_);
    open_  = false;
    state_ = State::Closed;
    items_.clear();
    logs_.clear();
    currentStatus_.clear();
    errorMessage_.clear();
    interruptButtonLabel_.clear();
    interruptConfirmTitle_.clear();
    interruptConfirmBody_.clear();
    abortedByUser_ = false;
    abortRequested_.store(false);
    interruptConfirm_.close();
    sourceCallbacks_.reset();
}

bool InstallerScreen::shouldRefreshOnClose() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::Finished && deleteMode_ == InstallDeleteMode::DeleteAfterInstall;
}

std::vector<std::string> InstallerScreen::sourceDirectories() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> dirs;
    dirs.reserve(items_.size());
    for (const auto& item : items_) {
        std::string dir = fs::parentPath(item.path);
        if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end())
            dirs.push_back(std::move(dir));
    }
    return dirs;
}

void InstallerScreen::appendLog(std::string line) {
    if (line.empty())
        return;
#ifdef XXPLORE_DEBUG
    std::printf("[installer-ui] %s\n", line.c_str());
#endif
    static constexpr size_t kMaxLogs = 30;
    std::lock_guard<std::mutex> lock(mutex_);
    if (logs_.size() >= kMaxLogs)
        logs_.pop_front();
    logs_.push_back(std::move(line));
}

void InstallerScreen::joinWorker() {
    if (worker_.joinable())
        worker_.join();
}

void InstallerScreen::startInstallWorker() {
    joinWorker();

    std::vector<InstallQueueItem> items;
    bool installToNand = false;
    bool deleteAfterInstall = false;
    std::shared_ptr<InstallDataSourceCallbacks> sourceCallbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        items = items_;
        installToNand = target_ == InstallTarget::Nand;
        deleteAfterInstall = deleteMode_ == InstallDeleteMode::DeleteAfterInstall;
        sourceCallbacks = sourceCallbacks_;
        state_ = State::Running;
        currentProgress_ = 0.0f;
        totalProgress_ = 0.0f;
        currentStatus_.clear();
        errorMessage_.clear();
        abortedByUser_ = false;
        abortRequested_.store(false);
        interruptConfirm_.close();
        speedMeter_.reset();
    }

    worker_ = std::thread([this, items = std::move(items), installToNand, deleteAfterInstall,
                           sourceCallbacks = std::move(sourceCallbacks)]() mutable {
        util::acquireScreenAwake();
        struct ScreenAwakeRelease {
            ~ScreenAwakeRelease() { util::releaseScreenAwake(); }
        } screenAwakeRelease;

        InstallBackendCallbacks callbacks;
        callbacks.onLog = [this](const std::string& line) {
            appendLog(line);
        };
        callbacks.onStatus = [this](const std::string& status) {
            std::lock_guard<std::mutex> lock(mutex_);
            currentStatus_ = status;
        };
        callbacks.onProgress = [this](float currentProgress, float totalProgress) {
            std::lock_guard<std::mutex> lock(mutex_);
            currentProgress_ = currentProgress;
            totalProgress_ = totalProgress;
        };
        callbacks.onProgressBytes = [this](uint64_t currentItemBytes, uint64_t currentItemTotal,
                                           uint64_t totalBytesDone,
                                           uint64_t totalBytesTotal) {
            (void)currentItemBytes;
            (void)currentItemTotal;
            (void)totalBytesTotal;
            std::lock_guard<std::mutex> lock(mutex_);
            speedMeter_.update(totalBytesDone, static_cast<uint64_t>(std::time(nullptr)));
        };
        callbacks.shouldAbort = [this]() {
            return abortRequested_.load();
        };

        std::string err;
        bool ok = runInstallQueue(items, installToNand, deleteAfterInstall, callbacks, err,
                                  sourceCallbacks.get());
        bool abortedByUser = !ok && abortRequested_.load();

        std::lock_guard<std::mutex> lock(mutex_);
        currentProgress_ = ok ? 1.0f : currentProgress_;
        totalProgress_ = ok ? 1.0f : totalProgress_;
        currentStatus_.clear();
        abortedByUser_ = abortedByUser;
        if (ok) {
            state_ = State::Finished;
            speedMeter_.markFinished(static_cast<uint64_t>(std::time(nullptr)));
            if (logs_.size() >= 30)
                logs_.pop_front();
            logs_.push_back("Install completed.");
        } else {
            state_ = State::Failed;
            errorMessage_ = abortedByUser ? "Install aborted by user." : std::move(err);
            if (!errorMessage_.empty()) {
                if (logs_.size() >= 30)
                    logs_.pop_front();
                logs_.push_back("Install failed: " + errorMessage_);
            }
        }
    });
}

void InstallerScreen::update() {
    std::string queuedName;
    bool finishLoading = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_ || state_ != State::Loading)
            return;

        if (loadingIndex_ < items_.size()) {
            totalBytes_ += items_[loadingIndex_].size;
            queuedName = items_[loadingIndex_].name;
            loadingIndex_++;
        } else {
            finishLoading = true;
        }
    }

    if (!queuedName.empty()) {
        appendLog("Queued: " + queuedName);
        return;
    }

    if (!finishLoading)
        return;

    int64_t nandFree = -1;
    int64_t sdFree = -1;
    bool queryFailed = false;
    Result rc = nsInitialize();
    if (R_SUCCEEDED(rc)) {
        s64 freeBytes = 0;
        if (R_SUCCEEDED(nsGetFreeSpaceSize(NcmStorageId_BuiltInUser, &freeBytes)))
            nandFree = static_cast<int64_t>(freeBytes);
        if (R_SUCCEEDED(nsGetFreeSpaceSize(NcmStorageId_SdCard, &freeBytes)))
            sdFree = static_cast<int64_t>(freeBytes);
        nsExit();
    } else {
        queryFailed = true;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_ || state_ != State::Loading)
            return;
        nandFree_ = nandFree;
        sdFree_ = sdFree;
        state_ = State::Ready;
    }

    if (queryFailed)
        appendLog("Failed to query storage space.");
    appendLog("Install queue ready.");
}

InstallerAction InstallerScreen::handleInput(uint64_t kDown, const TouchTap* tap) {
    bool shouldStartInstall = false;
    State state = State::Closed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_)
            return InstallerAction::None;
        state = state_;
    }

    if (interruptConfirm_.isOpen() && state != State::Running)
        interruptConfirm_.close();

    if (interruptConfirm_.isOpen()) {
        ConfirmResult result = interruptConfirm_.handleInput(kDown, tap);
        if (result == ConfirmResult::Confirmed) {
            abortRequested_.store(true);
            appendLog("Install interrupt requested.");
            interruptConfirm_.close();
        } else if (result == ConfirmResult::Cancelled) {
            interruptConfirm_.close();
        }
        return InstallerAction::None;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_)
            return InstallerAction::None;

        if (state_ == State::Loading)
            return InstallerAction::None;

        if (state_ == State::Running)
            state = State::Running;

        if (state_ == State::Finished) {
            if (tap && tap->active &&
                ui::panelCloseButtonHit(40, 40, theme::SCREEN_W - 80, tap->x, tap->y)) {
                return InstallerAction::Close;
            }
            if (kDown & HidNpadButton_Plus)
                return InstallerAction::Close;
            return InstallerAction::None;
        }

        if (state_ == State::Failed) {
            if (tap && tap->active &&
                ui::panelCloseButtonHit(40, 40, theme::SCREEN_W - 80, tap->x, tap->y)) {
                return InstallerAction::Close;
            }
            if (kDown & (HidNpadButton_B | HidNpadButton_Plus))
                return InstallerAction::Close;
            return InstallerAction::None;
        }

        const int cardX = 40;
        const int cardY = 40;
        const int x = cardX + 24;
        const int summaryY = cardY + 20 + 34 + 40;
        const int targetW = 180;
        const int targetH = 42;
        const int targetGap = 16;
        const int targetsY = summaryY + 22;
        const int buttonsY = targetsY + targetH + 28;

        if (tap && tap->active && state_ == State::Ready) {
            if (pointInRect(tap, x, targetsY, targetW, targetH)) {
                focusRow_ = 0;
                targetFocusCol_ = 0;
                target_ = InstallTarget::Nand;
                return InstallerAction::None;
            }
            if (pointInRect(tap, x + targetW + targetGap, targetsY, targetW, targetH)) {
                focusRow_ = 0;
                targetFocusCol_ = 1;
                target_ = InstallTarget::SdCard;
                return InstallerAction::None;
            }
            if (pointInRect(tap, x, buttonsY, targetW, targetH)) {
                focusRow_ = 1;
                buttonFocusCol_ = 0;
                return InstallerAction::Close;
            }
            if (pointInRect(tap, x + targetW + targetGap, buttonsY, targetW, targetH)) {
                focusRow_ = 1;
                buttonFocusCol_ = 1;
                shouldStartInstall = true;
            }
        }

        if (state_ == State::Running) {
            if (kDown & HidNpadButton_Minus) {
                interruptConfirm_.open(interruptConfirmTitle_, interruptConfirmBody_);
                return InstallerAction::None;
            }
            if (tap && tap->active &&
                ui::panelTextButtonHit(40, 40, theme::SCREEN_W - 80, kTitleActionButtonW,
                                       tap->x, tap->y)) {
                interruptConfirm_.open(interruptConfirmTitle_, interruptConfirmBody_);
                return InstallerAction::None;
            }
            return InstallerAction::None;
        }

        if (kDown & HidNpadButton_B)
            return InstallerAction::Close;

        if (kDown & (HidNpadButton_AnyUp | HidNpadButton_AnyDown))
            focusRow_ = 1 - focusRow_;

        if (kDown & HidNpadButton_AnyLeft) {
            if (focusRow_ == 0) {
                if (targetFocusCol_ > 0)
                    targetFocusCol_--;
            } else if (buttonFocusCol_ > 0) {
                buttonFocusCol_--;
            }
        }
        if (kDown & HidNpadButton_AnyRight) {
            if (focusRow_ == 0) {
                if (targetFocusCol_ < 1)
                    targetFocusCol_++;
            } else if (buttonFocusCol_ < 1) {
                buttonFocusCol_++;
            }
        }

        target_ = (targetFocusCol_ == 0) ? InstallTarget::Nand : InstallTarget::SdCard;

        if (kDown & HidNpadButton_A) {
            if (focusRow_ == 0) {
                target_ = (targetFocusCol_ == 0) ? InstallTarget::Nand : InstallTarget::SdCard;
            } else if (buttonFocusCol_ == 0) {
                return InstallerAction::Close;
            } else {
                shouldStartInstall = true;
            }
        }
    }

    if (shouldStartInstall) {
        appendLog(target_ == InstallTarget::Nand ? "Target: NAND" : "Target: SD Card");
        if (deleteMode_ == InstallDeleteMode::DeleteAfterInstall)
            appendLog("Delete source files after install: enabled.");
        startInstallWorker();
    }

    return InstallerAction::None;
}

void InstallerScreen::render(Renderer& renderer, FontManager& fm, const I18n& i18n) const {
    bool open = false;
    State state = State::Closed;
    std::vector<InstallQueueItem> items;
    uint64_t totalBytes = 0;
    int64_t nandFree = -1;
    int64_t sdFree = -1;
    InstallTarget target = InstallTarget::SdCard;
    int focusRow = 1;
    int targetFocusCol = 1;
    int buttonFocusCol = 0;
    std::deque<std::string> logs;
    float currentProgress = 0.0f;
    float totalProgress = 0.0f;
    std::string currentStatus;
    std::string errorMessage;
    bool abortedByUser = false;
    uint64_t speedBytesPerSec = 0;
    bool hasSpeedSample = false;
    uint64_t transferredBytes = 0;
    bool speedFinished = false;
    uint64_t nowSec = static_cast<uint64_t>(std::time(nullptr));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        open = open_;
        state = state_;
        items = items_;
        totalBytes = totalBytes_;
        nandFree = nandFree_;
        sdFree = sdFree_;
        target = target_;
        focusRow = focusRow_;
        targetFocusCol = targetFocusCol_;
        buttonFocusCol = buttonFocusCol_;
        logs = logs_;
        currentProgress = currentProgress_;
        totalProgress = totalProgress_;
        currentStatus = currentStatus_;
        errorMessage = errorMessage_;
        abortedByUser = abortedByUser_;
        speedBytesPerSec = speedMeter_.rateBytesPerSec(nowSec);
        hasSpeedSample = speedMeter_.hasRate(nowSec);
        transferredBytes = speedMeter_.latestTotalBytes();
        speedFinished = speedMeter_.finished();
    }
    if (!open)
        return;

    renderer.drawRectFilled(0, 0, theme::SCREEN_W, theme::SCREEN_H, theme::BG);

    int cardX = 40;
    int cardY = 40;
    int cardW = theme::SCREEN_W - 80;
    int cardH = theme::SCREEN_H - 80;

    renderer.drawRoundedRectFilled(cardX, cardY, cardW, cardH, 16, theme::MENU_BG);
    renderer.drawRoundedRect(cardX, cardY, cardW, cardH, 16, theme::MENU_BORDER);

    int x = cardX + 24;
    int y = cardY + 20;

    fm.drawText(renderer.sdl(), i18n.t("installer.title"), x, y, theme::FONT_SIZE_TITLE,
                theme::PRIMARY);
    if (state == State::Finished || state == State::Failed)
        ui::drawPanelCloseButton(renderer, cardX, cardY, cardW, false);
    else if (state == State::Running)
        ui::drawPanelTextButton(renderer, fm, cardX, cardY, cardW, kTitleActionButtonW,
                                interruptButtonLabel_.c_str(), false);
    y += 34;

    if (state == State::Loading) {
        std::string loading = i18n.t("installer.loading");
        if (!items.empty()) {
            loading += " ";
            int loadingIndex = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                loadingIndex = static_cast<int>(loadingIndex_);
            }
            loading += std::to_string(loadingIndex);
            loading += "/";
            loading += std::to_string(items.size());
        }
        fm.drawText(renderer.sdl(), loading.c_str(), x, y, theme::FONT_SIZE_ITEM, theme::TEXT);
        return;
    }

    std::string summary = i18n.t("installer.summary_prefix");
    summary += " ";
    summary += std::to_string(items.size());
    summary += " ";
    summary += i18n.t(items.size() == 1 ? "installer.file_single" : "installer.file_plural");
    summary += ", ";
    summary += fs::formatSize(totalBytes);
    summary += ". ";
    summary += i18n.t("installer.nand_free");
    summary += " ";
    summary += nandFree >= 0 ? fs::formatSize(static_cast<uint64_t>(nandFree)) : "--";
    summary += ". ";
    summary += i18n.t("installer.sd_free");
    summary += " ";
    summary += sdFree >= 0 ? fs::formatSize(static_cast<uint64_t>(sdFree)) : "--";
    summary += ".";
    fm.drawTextEllipsis(renderer.sdl(), summary.c_str(), x, y, theme::FONT_SIZE_ITEM,
                        theme::TEXT, cardW - 48);
    y += 40;

    const int targetW = 180;
    const int targetH = 42;
    const int targetGap = 16;
    int logY = y;
    int logH = 0;
    if (state == State::Ready) {
        fm.drawText(renderer.sdl(), i18n.t("installer.target"), x, y, theme::FONT_SIZE_SMALL,
                    theme::TEXT_SECONDARY);
        if (showCompressedAppletWarning_) {
            int hintX = x + fm.measureText(i18n.t("installer.target"), theme::FONT_SIZE_SMALL) + 18;
            int hintW = cardW - 48 - (hintX - x);
            if (hintW > 24) {
                fm.drawTextEllipsis(renderer.sdl(), i18n.t("installer.applet_compressed_warning"),
                                    hintX, y, theme::FONT_SIZE_SMALL, kAppletWarningYellow, hintW);
            }
        }
        y += 22;

        struct TargetCell {
            const char* label;
            bool        active;
            bool        focused;
            int         x;
        };
        const TargetCell targets[] = {
            {i18n.t("installer.target_nand"), target == InstallTarget::Nand,
             focusRow == 0 && targetFocusCol == 0, x},
            {i18n.t("installer.target_sd"), target == InstallTarget::SdCard,
             focusRow == 0 && targetFocusCol == 1, x + targetW + targetGap},
        };
        for (const auto& cell : targets) {
            SDL_Color bg = theme::SURFACE;
            SDL_Color border = theme::DIVIDER;
            SDL_Color text = theme::TEXT;

            if (cell.active) {
                bg = withAlpha(theme::PRIMARY, cell.focused ? 0xff : 0x66);
                border = cell.focused ? theme::PRIMARY : theme::PRIMARY_DIM;
                text = cell.focused ? theme::ON_PRIMARY : theme::PRIMARY;
            } else if (cell.focused) {
                bg = theme::SURFACE_HOVER;
                border = theme::PRIMARY;
            }

            renderer.drawRoundedRectFilled(cell.x, y, targetW, targetH, 10, bg);
            renderer.drawRoundedRect(cell.x, y, targetW, targetH, 10, border);
            fm.drawText(renderer.sdl(), cell.label, cell.x + 16,
                        y + (targetH - theme::FONT_SIZE_ITEM) / 2, theme::FONT_SIZE_ITEM, text);
        }
        y += targetH + 28;

        int buttonY = y;
        struct ButtonCell {
            const char* label;
            bool        focused;
            int         x;
        };
        const ButtonCell buttons[] = {
            {i18n.t("installer.cancel"), focusRow == 1 && buttonFocusCol == 0, x},
            {i18n.t("installer.install"), focusRow == 1 && buttonFocusCol == 1, x + targetW + targetGap},
        };
        for (const auto& cell : buttons) {
            SDL_Color bg = cell.focused ? theme::PRIMARY_DIM : theme::SURFACE;
            SDL_Color border = cell.focused ? theme::PRIMARY : theme::DIVIDER;
            SDL_Color text = cell.focused ? theme::ON_PRIMARY : theme::TEXT;

            renderer.drawRoundedRectFilled(cell.x, buttonY, targetW, targetH, 10, bg);
            renderer.drawRoundedRect(cell.x, buttonY, targetW, targetH, 10, border);
            fm.drawText(renderer.sdl(), cell.label, cell.x + 16,
                        buttonY + (targetH - theme::FONT_SIZE_ITEM) / 2, theme::FONT_SIZE_ITEM, text);
        }
        logY = buttonY + targetH + 20;
        logH = 240;
    } else {
        std::string targetLine = std::string(i18n.t("installer.target")) + " "
                               + (target == InstallTarget::Nand ? i18n.t("installer.target_nand")
                                                                : i18n.t("installer.target_sd"));
        fm.drawText(renderer.sdl(), targetLine.c_str(), x, y, theme::FONT_SIZE_SMALL,
                    theme::TEXT_SECONDARY);
        y += 28;
        if (!currentStatus.empty()) {
            fm.drawTextEllipsis(renderer.sdl(), currentStatus.c_str(), x, y, theme::FONT_SIZE_SMALL,
                                theme::TEXT, cardW - 48);
            y += 28;
        }
        logY = y;
        int barsH = 18 + 20 + 18 + 20 + 34 + 24;
        int bottomReserved = barsH + 28;
        logH = std::max(180, cardY + cardH - bottomReserved - logY);
    }

    renderer.drawRoundedRectFilled(x, logY, cardW - 48, logH, 10, theme::BG);
    renderer.drawRoundedRect(x, logY, cardW - 48, logH, 10, theme::DIVIDER);
    fm.drawText(renderer.sdl(), i18n.t("installer.console"), x + 12, logY + 10,
                theme::FONT_SIZE_SMALL, theme::TEXT_SECONDARY);
    int lineY = logY + 36;
    int lineH = fm.fontHeight(theme::FONT_SIZE_SMALL) + 4;
    int maxVisible = (logH - 48) / lineH;
    int startIndex = 0;
    if (static_cast<int>(logs.size()) > maxVisible)
        startIndex = static_cast<int>(logs.size()) - maxVisible;
    for (int i = startIndex; i < static_cast<int>(logs.size()); i++) {
        const std::string& line = logs[static_cast<size_t>(i)];
        fm.drawTextEllipsis(renderer.sdl(), line.c_str(), x + 12, lineY, theme::FONT_SIZE_SMALL,
                            theme::TEXT, cardW - 72);
        lineY += lineH;
    }

    int barY = logY + logH + 24;
    if (state != State::Ready) {
        fm.drawText(renderer.sdl(), i18n.t("installer.current_progress"), x, barY,
                    theme::FONT_SIZE_SMALL, theme::TEXT_SECONDARY);
        barY += 20;
        drawProgressBar(renderer, x, barY, cardW - 48, 18, currentProgress);
        barY += 34;
        fm.drawText(renderer.sdl(), i18n.t("installer.total_progress"), x, barY,
                    theme::FONT_SIZE_SMALL, theme::TEXT_SECONDARY);
        barY += 20;
        drawProgressBar(renderer, x, barY, cardW - 48, 18, totalProgress);
    }

    if (state == State::Finished) {
        fm.drawText(renderer.sdl(), i18n.t("installer.exit_hint"), x, cardY + cardH - 32,
                    theme::FONT_SIZE_SMALL, theme::TEXT_SECONDARY);
        const char* speedText = i18n.t("installer.speed_done");
        int speedW = fm.measureText(speedText, theme::FONT_SIZE_SMALL);
        fm.drawText(renderer.sdl(), speedText, x + (cardW - 48) - speedW, cardY + cardH - 32,
                    theme::FONT_SIZE_SMALL, theme::TEXT_SECONDARY);
    } else if (state == State::Failed) {
        fm.drawTextEllipsis(renderer.sdl(), i18n.t("installer.failed_cleanup_hint"), x,
                            cardY + cardH - 52, theme::FONT_SIZE_SMALL, theme::TEXT_SECONDARY,
                            cardW - 48);
        if (!errorMessage.empty()) {
            const char* displayError = abortedByUser ? i18n.t("installer.abort_result")
                                                     : errorMessage.c_str();
            fm.drawTextEllipsis(renderer.sdl(), displayError, x + 220, cardY + cardH - 30,
                                theme::FONT_SIZE_SMALL, theme::DANGER, cardW - 260);
        }
        fm.drawText(renderer.sdl(), i18n.t("installer.failed_hint"), x, cardY + cardH - 30,
                    theme::FONT_SIZE_SMALL, theme::DANGER);
    } else if (state == State::Running) {
        std::string speedText;
        if (speedFinished)
            speedText = i18n.t("installer.speed_done");
        else if (hasSpeedSample)
            speedText = formatInstallSpeed(speedBytesPerSec);
        else if (transferredBytes == 0)
            speedText = i18n.t("installer.speed_pending");
        else
            speedText = fs::formatSize(transferredBytes);
        int speedW = fm.measureText(speedText.c_str(), theme::FONT_SIZE_SMALL);
        fm.drawText(renderer.sdl(), speedText.c_str(),
                    x + (cardW - 48) - speedW, cardY + cardH - 32,
                    theme::FONT_SIZE_SMALL, theme::TEXT_SECONDARY);
    }

    interruptConfirm_.render(renderer, fm, i18n);
}

} // namespace xxplore
