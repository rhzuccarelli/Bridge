#include "aw/ui/screens/ExportScreen.hpp"

#include "aw/display/Color.hpp"
#include "aw/display/Font.hpp"

#include <algorithm>
#include <cctype>

namespace aw::ui {

ExportScreen::ExportScreen(mtp::MtpManager& manager, mtp::MtpBrowser& browser,
                           project::ProjectManager& projects,
                           project::ExportService& exporter, TaskRunner& tasks,
                           events::EventQueue& workerQueue,
                           hal::DeviceModeInterface* deviceMode) noexcept
    : manager_{manager}, browser_{browser}, projects_{projects}, exporter_{exporter}, tasks_{tasks},
      workerQueue_{workerQueue}, deviceMode_{deviceMode} {}

void ExportScreen::onEnter() {
    selection_ = 0; progressPercent_ = 0; waitingMillis_ = 0;
    lastError_ = core::ErrorCode::Unknown; storageId_.store(0, std::memory_order_release);
    folderId_.store(hal::kMtpRootHandle, std::memory_order_release);
    if (manager_.state() == mtp::ConnectionState::Detected ||
        manager_.state() == mtp::ConnectionState::Connected) {
        startScan();
    } else {
        requestMtpMode();
    }
    dirty_.set();
}

void ExportScreen::onExit() {
    if (stage_ != Stage::Transferring && manager_.state() == mtp::ConnectionState::Connected) {
        (void)tasks_.submit([this] { manager_.disconnect(); });
    }
}

void ExportScreen::requestMtpMode() {
    if (deviceMode_ == nullptr) {
        stage_ = Stage::Selecting;
        lastError_ = core::ErrorCode::DeviceNotConnected;
        return;
    }
    stage_ = Stage::SwitchingMode;
    const std::uint32_t modeId = ++jobId_;
    const core::Status accepted = tasks_.submit([this, modeId] {
        const core::Status result = deviceMode_->requestMtpMode();
        workerQueue_.post(events::JobFinishedEvent{
            events::JobKind::DeviceMode, modeId, static_cast<bool>(result),
            result ? core::ErrorCode::Unknown : result.error().code()});
    });
    if (!accepted) {
        stage_ = Stage::Selecting;
        lastError_ = accepted.error().code();
    }
}

void ExportScreen::startScan() {
    if (stage_ == Stage::Connecting) return;
    stage_ = Stage::Connecting;
    const std::uint32_t scanId = ++jobId_;
    const core::Status accepted = tasks_.submit([this, scanId] {
        core::Status result = manager_.connect();
        if (result) {
            auto storages = manager_.storages();
            if (!storages) result = storages.error();
            else if (storages.value().empty())
                result = core::makeError(core::ErrorCode::NotFound, "TP-7 has no storage");
            else {
                storageId_.store(storages.value().front().id, std::memory_order_release);
                result = browser_.openStorage(storages.value().front().id);
                if (result) {
                    for (const auto& entry : browser_.entries()) {
                        std::string lower = entry.name;
                        std::transform(lower.begin(), lower.end(), lower.begin(),
                            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        if (entry.isFolder && lower == "recordings") {
                            folderId_.store(entry.handle, std::memory_order_release);
                            break;
                        }
                    }
                }
            }
        }
        workerQueue_.post(events::JobFinishedEvent{events::JobKind::Scan, scanId,
            static_cast<bool>(result), result ? core::ErrorCode::Unknown : result.error().code()});
    });
    if (!accepted) {
        stage_ = Stage::Selecting;
        lastError_ = accepted.error().code();
    }
}

NavigationRequest ExportScreen::handleEvent(const events::Event& event) {
    if (const auto* device = std::get_if<events::DeviceEvent>(&event)) {
        if (device->kind == events::DeviceKind::Mtp && device->attached) {
            startScan();
            dirty_.set();
            return NavigationRequest::none();
        }
    }
    if (const auto* progress = std::get_if<events::JobProgressEvent>(&event)) {
        if (progress->kind == events::JobKind::Export) {
            if (stage_ == Stage::Checking) stage_ = Stage::Transferring;
            progressPercent_ = progress->percent; dirty_.set(); return NavigationRequest::none();
        }
    }
    if (const auto* finished = std::get_if<events::JobFinishedEvent>(&event)) {
        if (finished->kind == events::JobKind::DeviceMode && finished->jobId == jobId_) {
            stage_ = finished->succeeded ? Stage::WaitingForMtp : Stage::Selecting;
            waitingMillis_ = 0;
            lastError_ = finished->succeeded ? core::ErrorCode::Unknown : finished->error;
            dirty_.set();
            return NavigationRequest::none();
        }
        if (finished->kind == events::JobKind::Scan && finished->jobId == jobId_) {
            if (stage_ == Stage::Connecting) {
                stage_ = Stage::Selecting;
                lastError_ = finished->succeeded ? core::ErrorCode::Unknown : finished->error;
            } else if (stage_ == Stage::Checking) {
                stage_ = finished->error == core::ErrorCode::AlreadyExists
                             ? Stage::ConfirmingOverwrite : Stage::Selecting;
                lastError_ = finished->error == core::ErrorCode::AlreadyExists
                                 ? core::ErrorCode::Unknown : finished->error;
            }
            dirty_.set(); return NavigationRequest::none();
        }
        if (finished->kind == events::JobKind::Export && finished->jobId == jobId_) {
            stage_ = Stage::Selecting;
            lastError_ = finished->succeeded ? core::ErrorCode::Unknown : finished->error;
            dirty_.set();
            return finished->succeeded ? NavigationRequest::back() : NavigationRequest::none();
        }
    }

    if (const auto* button = std::get_if<events::ButtonEvent>(&event)) {
        if (button->action != core::ButtonAction::Pressed) return NavigationRequest::ignored();
        if (stage_ == Stage::Transferring) {
            if (button->id == core::ButtonId::Stop) exporter_.requestCancel();
            return NavigationRequest::none();
        }
        if (stage_ == Stage::ConfirmingOverwrite) {
            if (button->id == core::ButtonId::Back) {
                stage_ = Stage::Selecting; dirty_.set(); return NavigationRequest::none();
            }
            if (button->id == core::ButtonId::Export ||
                button->id == core::ButtonId::EncoderPush) {
                project::ExportPlan plan;
                plan.targetStorage = storageId_.load(std::memory_order_acquire);
                plan.targetFolder = folderId_.load(std::memory_order_acquire);
                plan.conflictPolicy = project::ExportConflictPolicy::Overwrite;
                const std::uint32_t exportId = ++jobId_;
                stage_ = Stage::Transferring; progressPercent_ = 0;
                (void)tasks_.submit([this, exportId, plan = std::move(plan)] {
                    (void)exporter_.run(plan, exportId);
                    manager_.disconnect();
                });
            }
            return NavigationRequest::none();
        }
        if (stage_ == Stage::Selecting &&
            (button->id == core::ButtonId::Export ||
             button->id == core::ButtonId::EncoderPush)) {
            if (storageId_.load(std::memory_order_acquire) == 0) {
                onEnter();
                return NavigationRequest::none();
            }
            project::ExportPlan plan;
            plan.targetStorage = storageId_.load(std::memory_order_acquire);
            plan.targetFolder = folderId_.load(std::memory_order_acquire);
            plan.conflictPolicy = project::ExportConflictPolicy::Fail;
            const std::uint32_t checkId = ++jobId_;
            stage_ = Stage::Checking; lastError_ = core::ErrorCode::Unknown;
            (void)tasks_.submit([this, checkId, plan = std::move(plan)] {
                auto conflicts = exporter_.findConflicts(plan);
                if (!conflicts || !conflicts.value().empty()) {
                    const auto error = !conflicts ? conflicts.error().code() : core::ErrorCode::AlreadyExists;
                    workerQueue_.post(events::JobFinishedEvent{events::JobKind::Scan, checkId, false, error});
                } else {
                    (void)exporter_.run(plan, checkId);
                    manager_.disconnect();
                }
            });
            dirty_.set(); return NavigationRequest::none();
        }
    }
    if (stage_ == Stage::SwitchingMode || stage_ == Stage::WaitingForMtp ||
        stage_ == Stage::Connecting || stage_ == Stage::Checking)
        return NavigationRequest::none();
    return NavigationRequest::ignored();
}

void ExportScreen::update(std::uint32_t elapsedMillis) {
    if (stage_ != Stage::WaitingForMtp) return;
    waitingMillis_ += elapsedMillis;
    if (waitingMillis_ >= 8000) {
        stage_ = Stage::Selecting;
        lastError_ = core::ErrorCode::DeviceNotConnected;
        dirty_.set();
    }
}

void ExportScreen::render(display::Painter& painter) {
    painter.clear(display::palette::kBackground);
    const auto& font = display::smallFont();
    painter.fillRect({0, 0, 128, 12}, display::palette::kSurface);
    painter.drawText(2, 0, "EXPORT TP-7", font, display::palette::kText);
    if (stage_ == Stage::SwitchingMode) {
        painter.drawTextIn({0, 20, 128, 12}, "STARTING MTP", font,
                           display::palette::kText, display::TextAlign::Center);
        painter.drawTextIn({0, 38, 128, 12}, "TP-7 RECONNECTING", font,
                           display::palette::kTextDim, display::TextAlign::Center);
    } else if (stage_ == Stage::WaitingForMtp) {
        painter.drawTextIn({0, 24, 128, 12}, "WAITING FOR TP-7", font,
                           display::palette::kText, display::TextAlign::Center);
    } else if (stage_ == Stage::Connecting || stage_ == Stage::Checking) {
        painter.drawTextIn({0, 24, 128, 12}, stage_ == Stage::Connecting ? "CONNECTING" : "CHECKING NAME",
                           font, display::palette::kText, display::TextAlign::Center);
    } else if (stage_ == Stage::Transferring) {
        painter.drawTextIn({0, 16, 128, 12}, progressPercent_ < 50 ? "RENDERING 48K" : "SENDING TP-7",
                           font, display::palette::kText, display::TextAlign::Center);
        painter.drawMeter({8, 34, 112, 8}, static_cast<float>(progressPercent_) / 100.0F, false);
        painter.drawTextIn({0, 48, 128, 12}, "STOP:CANCEL", font,
                           display::palette::kTextDim, display::TextAlign::Center);
    } else if (stage_ == Stage::ConfirmingOverwrite) {
        painter.drawTextIn({0, 14, 128, 12}, "FILE EXISTS", font,
                           display::palette::kText, display::TextAlign::Center);
        painter.drawTextIn({0, 28, 128, 12}, "EXPORT:REPLACE", font,
                           display::palette::kText, display::TextAlign::Center);
        painter.drawTextIn({0, 44, 128, 12}, "BACK:CANCEL", font,
                           display::palette::kTextDim, display::TextAlign::Center);
    } else if (lastError_ != core::ErrorCode::Unknown) {
        painter.drawTextIn({0, 22, 128, 12}, core::toString(lastError_), font,
                           display::palette::kText, display::TextAlign::Center);
    } else {
        const std::string_view deck = projects_.hasProject()
                                          ? projects_.project().activeDeck().name
                                          : std::string_view{"NO PROJECT"};
        painter.drawTextIn({0, 14, 128, 12}, deck.substr(0, 15), font,
                           display::palette::kText, display::TextAlign::Center);
        painter.drawTextIn({0, 29, 128, 12}, "48K / UP TO 12CH", font,
                           display::palette::kTextDim, display::TextAlign::Center);
        painter.drawTextIn({0, 46, 128, 12}, "PUSH TO EXPORT", font,
                           display::palette::kText, display::TextAlign::Center);
    }
    dirty_.clear();
}

}  // namespace aw::ui
