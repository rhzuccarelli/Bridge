#include "aw/ui/screens/ExportScreen.hpp"

#include "aw/display/Color.hpp"
#include "aw/display/Font.hpp"

#include <algorithm>
#include <cctype>

namespace aw::ui {

ExportScreen::ExportScreen(mtp::MtpManager& manager, mtp::MtpBrowser& browser,
                           project::ProjectManager& projects,
                           project::ExportService& exporter, TaskRunner& tasks,
                           events::EventQueue& workerQueue) noexcept
    : manager_{manager}, browser_{browser}, projects_{projects}, exporter_{exporter}, tasks_{tasks},
      workerQueue_{workerQueue} {}

void ExportScreen::onEnter() {
    stage_ = Stage::Connecting; selection_ = 0; progressPercent_ = 0;
    lastError_ = core::ErrorCode::Unknown; storageId_.store(0, std::memory_order_release);
    folderId_.store(hal::kMtpRootHandle, std::memory_order_release);
    const std::uint32_t scanId = ++jobId_;
    (void)tasks_.submit([this, scanId] {
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
    dirty_.set();
}

NavigationRequest ExportScreen::handleEvent(const events::Event& event) {
    if (const auto* progress = std::get_if<events::JobProgressEvent>(&event)) {
        if (progress->kind == events::JobKind::Export) {
            if (stage_ == Stage::Checking) stage_ = Stage::Transferring;
            progressPercent_ = progress->percent; dirty_.set(); return NavigationRequest::none();
        }
    }
    if (const auto* finished = std::get_if<events::JobFinishedEvent>(&event)) {
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
            if (button->id == core::ButtonId::Export) {
                project::ExportPlan plan;
                plan.targetStorage = storageId_.load(std::memory_order_acquire);
                plan.targetFolder = folderId_.load(std::memory_order_acquire);
                plan.conflictPolicy = project::ExportConflictPolicy::Overwrite;
                const std::uint32_t exportId = ++jobId_;
                stage_ = Stage::Transferring; progressPercent_ = 0;
                (void)tasks_.submit([this, exportId, plan = std::move(plan)] {
                    (void)exporter_.run(plan, exportId);
                });
            }
            return NavigationRequest::none();
        }
        if (stage_ == Stage::Selecting && button->id == core::ButtonId::Export) {
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
                }
            });
            dirty_.set(); return NavigationRequest::none();
        }
    }
    if (stage_ == Stage::Connecting || stage_ == Stage::Checking)
        return NavigationRequest::none();
    return NavigationRequest::ignored();
}

void ExportScreen::render(display::Painter& painter) {
    painter.clear(display::palette::kBackground);
    const auto& font = display::smallFont();
    painter.fillRect({0, 0, 128, 10}, display::palette::kSurface);
    painter.drawText(2, 1, "EXPORT TP-7", font, display::palette::kText);
    if (stage_ == Stage::Connecting || stage_ == Stage::Checking) {
        painter.drawTextIn({0, 25, 128, 8}, stage_ == Stage::Connecting ? "CONNECTING..." : "CHECKING NAME...",
                           font, display::palette::kText, display::TextAlign::Center);
    } else if (stage_ == Stage::Transferring) {
        painter.drawTextIn({0, 18, 128, 8}, progressPercent_ < 50 ? "RENDERING 48K" : "SENDING TO TP-7",
                           font, display::palette::kText, display::TextAlign::Center);
        painter.drawMeter({8, 34, 112, 8}, static_cast<float>(progressPercent_) / 100.0F, false);
        painter.drawTextIn({0, 48, 128, 8}, "STOP: CANCEL", font,
                           display::palette::kTextDim, display::TextAlign::Center);
    } else if (stage_ == Stage::ConfirmingOverwrite) {
        painter.drawTextIn({0, 17, 128, 8}, "FILE EXISTS", font,
                           display::palette::kText, display::TextAlign::Center);
        painter.drawTextIn({0, 31, 128, 8}, "EXPORT: OVERWRITE", font,
                           display::palette::kText, display::TextAlign::Center);
        painter.drawTextIn({0, 44, 128, 8}, "BACK: CANCEL", font,
                           display::palette::kTextDim, display::TextAlign::Center);
    } else if (lastError_ != core::ErrorCode::Unknown) {
        painter.drawTextIn({0, 22, 128, 8}, core::toString(lastError_), font,
                           display::palette::kText, display::TextAlign::Center);
    } else {
        const std::string_view deck = projects_.hasProject()
                                          ? projects_.project().activeDeck().name
                                          : std::string_view{"NO PROJECT"};
        painter.drawTextIn({0, 17, 128, 8}, deck, font,
                           display::palette::kText, display::TextAlign::Center);
        painter.drawTextIn({0, 30, 128, 8}, "ONE 48K / UP TO 12CH", font,
                           display::palette::kTextDim, display::TextAlign::Center);
        painter.drawTextIn({0, 46, 128, 8}, "PRESS EXPORT", font,
                           display::palette::kText, display::TextAlign::Center);
    }
    dirty_.clear();
}

}  // namespace aw::ui
