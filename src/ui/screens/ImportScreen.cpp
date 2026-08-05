#include "aw/ui/screens/ImportScreen.hpp"

#include "aw/display/Color.hpp"

namespace aw::ui {

ImportScreen::ImportScreen(mtp::MtpManager& manager,
                           mtp::MtpBrowser& browser,
                           project::ImportService& importer) noexcept
    : manager_{manager}, browser_{browser}, importer_{importer} {}

void ImportScreen::onEnter() {
    selection_ = 0;
    scrollOffset_ = 0;
    transferInProgress_ = false;
    progressPercent_ = 0;
    dirty_.set();
    // Phase 8 kicks off connect + listing on the I/O thread here.  It is not
    // done on device attach: plugging a cable in must never start work.
}

NavigationRequest ImportScreen::handleEvent(const events::Event& event) {
    if (transferInProgress_) {
        if (const auto* progress = std::get_if<events::JobProgressEvent>(&event)) {
            progressPercent_ = progress->percent;
            dirty_.set();
            return NavigationRequest::none();
        }
        if (const auto* finished = std::get_if<events::JobFinishedEvent>(&event)) {
            transferInProgress_ = false;
            dirty_.set();
            return finished->succeeded ? NavigationRequest::back() : NavigationRequest::none();
        }
        // Stop cancels a running transfer; everything else is swallowed so the
        // user cannot navigate away from a transfer in progress.
        if (const auto* button = std::get_if<events::ButtonEvent>(&event)) {
            if (button->id == core::ButtonId::Stop &&
                button->action == core::ButtonAction::Pressed) {
                importer_.requestCancel();
            }
        }
        return NavigationRequest::none();
    }

    if (const auto* encoder = std::get_if<events::EncoderEvent>(&event)) {
        const int count = static_cast<int>(browser_.entries().size());
        const int next = selection_ + encoder->delta;
        selection_ = next < 0 ? 0 : (next >= count ? (count > 0 ? count - 1 : 0) : next);
        dirty_.set();
        return NavigationRequest::none();
    }

    if (const auto* button = std::get_if<events::ButtonEvent>(&event)) {
        if (button->action != core::ButtonAction::Pressed) {
            return NavigationRequest::ignored();
        }
        if (button->id == core::ButtonId::EncoderPush) {
            // Phase 8: enter folder, or toggle selection on a WAV.
            browser_.toggleSelection(static_cast<std::size_t>(selection_));
            dirty_.set();
            return NavigationRequest::none();
        }
        if (button->id == core::ButtonId::Import) {
            // Phase 8: build an ImportPlan from the selection and hand it to
            // the I/O worker.  Explicit commit, never automatic.
            (void)manager_;
            dirty_.set();
            return NavigationRequest::none();
        }
    }
    return NavigationRequest::ignored();
}

void ImportScreen::render(display::Painter& painter) {
    // Phase 7/8: breadcrumb, entry list with selection ticks, or a progress bar.
    painter.clear(display::palette::kBackground);
    dirty_.clear();
}

}  // namespace aw::ui
