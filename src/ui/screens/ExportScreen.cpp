#include "aw/ui/screens/ExportScreen.hpp"

#include "aw/display/Color.hpp"

namespace aw::ui {

ExportScreen::ExportScreen(mtp::MtpManager& manager,
                           project::ProjectManager& projects,
                           project::ExportService& exporter) noexcept
    : manager_{manager}, projects_{projects}, exporter_{exporter} {}

void ExportScreen::onEnter() {
    stage_ = Stage::Selecting;
    selection_ = 0;
    progressPercent_ = 0;
    dirty_.set();
}

NavigationRequest ExportScreen::handleEvent(const events::Event& event) {
    if (const auto* progress = std::get_if<events::JobProgressEvent>(&event)) {
        progressPercent_ = progress->percent;
        dirty_.set();
        return NavigationRequest::none();
    }
    if (const auto* finished = std::get_if<events::JobFinishedEvent>(&event)) {
        stage_ = Stage::Selecting;
        dirty_.set();
        return finished->succeeded ? NavigationRequest::back() : NavigationRequest::none();
    }

    if (const auto* button = std::get_if<events::ButtonEvent>(&event)) {
        if (button->action != core::ButtonAction::Pressed) {
            return NavigationRequest::ignored();
        }

        if (stage_ == Stage::ConfirmingOverwrite) {
            // Export confirms, Back cancels.  No other key does anything here:
            // an accidental press must not overwrite someone's recording.
            if (button->id == core::ButtonId::Export) {
                stage_ = Stage::Transferring;
                (void)exporter_;
                dirty_.set();
            } else if (button->id == core::ButtonId::Back) {
                stage_ = Stage::Selecting;
                dirty_.set();
            }
            return NavigationRequest::none();
        }

        if (button->id == core::ButtonId::Export) {
            // Phase 8: findConflicts, then either transfer or ask.
            (void)manager_;
            (void)projects_;
            dirty_.set();
            return NavigationRequest::none();
        }
    }

    if (const auto* encoder = std::get_if<events::EncoderEvent>(&event)) {
        selection_ = selection_ + encoder->delta;
        selection_ = selection_ < 0 ? 0 : selection_;
        dirty_.set();
        return NavigationRequest::none();
    }

    return NavigationRequest::ignored();
}

void ExportScreen::render(display::Painter& painter) {
    // Phase 7/8: track list with targets, or the overwrite confirmation, or a
    // progress bar.
    painter.clear(display::palette::kBackground);
    dirty_.clear();
}

}  // namespace aw::ui
