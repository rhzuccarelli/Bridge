#include "aw/ui/screens/ImportScreen.hpp"

#include "aw/display/Color.hpp"
#include "aw/display/Font.hpp"

#include <algorithm>

namespace aw::ui {

ImportScreen::ImportScreen(mtp::MtpManager& manager, mtp::MtpBrowser& browser,
                           project::ImportService& importer, TaskRunner& tasks,
                           events::EventQueue& workerQueue) noexcept
    : manager_{manager}, browser_{browser}, importer_{importer}, tasks_{tasks},
      workerQueue_{workerQueue} {}

void ImportScreen::onEnter() {
    selection_ = 0; scrollOffset_ = 0; transferInProgress_ = false; progressPercent_ = 0;
    lastError_ = core::ErrorCode::Unknown; scanning_ = true;
    const std::uint32_t scanId = ++jobId_;
    auto accepted = tasks_.submit([this, scanId] {
        core::Status result = manager_.connect();
        if (result) {
            auto storages = manager_.storages();
            if (!storages) result = storages.error();
            else if (storages.value().empty())
                result = core::makeError(core::ErrorCode::NotFound, "TP-7 has no MTP storage");
            else result = browser_.openStorage(storages.value().front().id);
        }
        workerQueue_.post(events::JobFinishedEvent{events::JobKind::Scan, scanId,
            static_cast<bool>(result), result ? core::ErrorCode::Unknown : result.error().code()});
    });
    if (!accepted) { scanning_ = false; lastError_ = accepted.error().code(); }
    dirty_.set();
}

NavigationRequest ImportScreen::handleEvent(const events::Event& event) {
    if (const auto* progress = std::get_if<events::JobProgressEvent>(&event)) {
        if (progress->kind == events::JobKind::Import) {
            progressPercent_ = progress->percent; dirty_.set(); return NavigationRequest::none();
        }
    }
    if (const auto* finished = std::get_if<events::JobFinishedEvent>(&event)) {
        if (finished->kind == events::JobKind::Scan && finished->jobId == jobId_) {
            scanning_ = false;
            lastError_ = finished->succeeded ? core::ErrorCode::Unknown : finished->error;
            selection_ = 0; scrollOffset_ = 0; dirty_.set(); return NavigationRequest::none();
        }
        if (finished->kind == events::JobKind::Import && finished->jobId == jobId_) {
            transferInProgress_ = false;
            lastError_ = finished->succeeded ? core::ErrorCode::Unknown : finished->error;
            dirty_.set();
            return finished->succeeded ? NavigationRequest::back() : NavigationRequest::none();
        }
    }
    if (transferInProgress_) {
        if (const auto* button = std::get_if<events::ButtonEvent>(&event))
            if (button->id == core::ButtonId::Stop && button->action == core::ButtonAction::Pressed)
                importer_.requestCancel();
        return NavigationRequest::none();
    }
    if (scanning_) return NavigationRequest::none();

    if (const auto* encoder = std::get_if<events::EncoderEvent>(&event)) {
        const int count = static_cast<int>(browser_.entries().size());
        selection_ = std::clamp(selection_ + encoder->delta, 0, std::max(0, count - 1));
        if (selection_ < scrollOffset_) scrollOffset_ = selection_;
        if (selection_ >= scrollOffset_ + 3) scrollOffset_ = selection_ - 2;
        dirty_.set(); return NavigationRequest::none();
    }
    if (const auto* button = std::get_if<events::ButtonEvent>(&event)) {
        if (button->action != core::ButtonAction::Pressed) return NavigationRequest::ignored();
        if (button->id == core::ButtonId::EncoderPush &&
            selection_ < static_cast<int>(browser_.entries().size())) {
            const auto entry = browser_.entries()[static_cast<std::size_t>(selection_)];
            if (entry.isFolder) {
                scanning_ = true; const std::uint32_t scanId = ++jobId_;
                (void)tasks_.submit([this, scanId, handle = entry.handle] {
                    core::Status result = browser_.enterFolder(handle);
                    workerQueue_.post(events::JobFinishedEvent{events::JobKind::Scan, scanId,
                        static_cast<bool>(result), result ? core::ErrorCode::Unknown : result.error().code()});
                });
            } else if (!entry.isSelected) {
                browser_.toggleSelection(static_cast<std::size_t>(selection_));
            } else {
                project::ImportPlan plan;
                for (const auto& selected : browser_.selectedEntries())
                    plan.items.push_back({selected.handle, selected.name, selected.sizeBytes,
                                          core::kInvalidTrackId});
                const std::uint32_t importId = ++jobId_;
                core::Status valid = importer_.validate(plan);
                if (!valid) lastError_ = valid.error().code();
                else {
                    transferInProgress_ = true; progressPercent_ = 0;
                    (void)tasks_.submit([this, importId, plan = std::move(plan)] {
                        (void)importer_.run(plan, importId);
                    });
                }
            }
            dirty_.set(); return NavigationRequest::none();
        }
        if (button->id == core::ButtonId::Back && browser_.canGoUp()) {
            scanning_ = true; const std::uint32_t scanId = ++jobId_;
            (void)tasks_.submit([this, scanId] {
                core::Status result = browser_.goUp();
                workerQueue_.post(events::JobFinishedEvent{events::JobKind::Scan, scanId,
                    static_cast<bool>(result), result ? core::ErrorCode::Unknown : result.error().code()});
            });
            dirty_.set(); return NavigationRequest::none();
        }
        if (button->id == core::ButtonId::Import) {
            project::ImportPlan plan;
            for (const auto& entry : browser_.selectedEntries())
                plan.items.push_back({entry.handle, entry.name, entry.sizeBytes, core::kInvalidTrackId});
            const std::uint32_t importId = ++jobId_;
            core::Status valid = importer_.validate(plan);
            if (!valid) lastError_ = valid.error().code();
            else {
                transferInProgress_ = true; progressPercent_ = 0;
                (void)tasks_.submit([this, importId, plan = std::move(plan)] {
                    (void)importer_.run(plan, importId);
                });
            }
            dirty_.set(); return NavigationRequest::none();
        }
    }
    return NavigationRequest::ignored();
}

void ImportScreen::render(display::Painter& painter) {
    painter.clear(display::palette::kBackground);
    const auto& font = display::smallFont();
    painter.fillRect({0, 0, 128, 12}, display::palette::kSurface);
    painter.drawText(2, 0, "IMPORT TP-7", font, display::palette::kText);
    if (scanning_) {
        painter.drawTextIn({0, 24, 128, 12}, "READING TP-7", font,
                           display::palette::kText, display::TextAlign::Center);
    } else if (transferInProgress_) {
        painter.drawTextIn({0, 16, 128, 12}, "COPYING", font,
                           display::palette::kText, display::TextAlign::Center);
        painter.drawMeter({8, 34, 112, 8}, static_cast<float>(progressPercent_) / 100.0F, false);
        painter.drawTextIn({0, 48, 128, 12}, "STOP:CANCEL", font,
                           display::palette::kTextDim, display::TextAlign::Center);
    } else if (lastError_ != core::ErrorCode::Unknown) {
        const std::string_view message = lastError_ == core::ErrorCode::InvalidState
                                             ? std::string_view{"OPEN PROJECT FIRST"}
                                             : core::toString(lastError_);
        painter.drawTextIn({0, 20, 128, 12}, message, font,
                           display::palette::kText, display::TextAlign::Center);
        painter.drawTextIn({0, 38, 128, 12}, "BACK / RETRY", font,
                           display::palette::kTextDim, display::TextAlign::Center);
    } else {
        std::string breadcrumb = browser_.breadcrumbText();
        if (breadcrumb.size() > 15) breadcrumb = breadcrumb.substr(breadcrumb.size() - 15);
        painter.drawText(2, 13, breadcrumb, font, display::palette::kTextDim);
        for (int row = 0; row < 3; ++row) {
            const int index = scrollOffset_ + row;
            if (index >= static_cast<int>(browser_.entries().size())) break;
            const auto& entry = browser_.entries()[static_cast<std::size_t>(index)];
            const auto y = static_cast<std::int16_t>(27 + row * 12);
            if (index == selection_) painter.drawRect({0, static_cast<std::int16_t>(y - 1), 128, 12},
                                                       display::palette::kAccent);
            painter.drawText(2, y, entry.isFolder ? ">" : (entry.isSelected ? "X" : " "),
                             font, display::palette::kText);
            std::string_view name = entry.name.size() > 13 ? std::string_view{entry.name}.substr(0, 13)
                                                           : std::string_view{entry.name};
            painter.drawText(12, y, name, font, display::palette::kText);
            if (index == selection_ && entry.isSelected)
                painter.drawTextIn({86, y, 40, 12}, "PUSH", font,
                                   display::palette::kText, display::TextAlign::Right);
        }
    }
    dirty_.clear();
}

}  // namespace aw::ui
