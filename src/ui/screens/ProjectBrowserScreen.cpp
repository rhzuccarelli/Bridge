#include "aw/ui/screens/ProjectBrowserScreen.hpp"

#include "aw/display/Color.hpp"

namespace aw::ui {

ProjectBrowserScreen::ProjectBrowserScreen(project::ProjectManager& projects) noexcept
    : projects_{projects} {}

void ProjectBrowserScreen::onEnter() {
    // Re-listing on entry rather than caching: a card can be swapped while the
    // device is running, and a stale list is worse than a short pause.
    if (auto listing = projects_.listProjects()) {
        entries_ = std::move(listing).value();
    } else {
        entries_.clear();
    }
    selection_ = 0;
    scrollOffset_ = 0;
    dirty_.set();
}

NavigationRequest ProjectBrowserScreen::handleEvent(const events::Event& event) {
    if (const auto* encoder = std::get_if<events::EncoderEvent>(&event)) {
        const int count = static_cast<int>(entries_.size());
        const int next = selection_ + encoder->delta;
        selection_ = next < 0 ? 0 : (next >= count ? (count > 0 ? count - 1 : 0) : next);
        dirty_.set();
        return NavigationRequest::none();
    }

    if (const auto* button = std::get_if<events::ButtonEvent>(&event)) {
        if (button->action == core::ButtonAction::Pressed &&
            button->id == core::ButtonId::EncoderPush) {
            if (selection_ >= 0 && selection_ < static_cast<int>(entries_.size())) {
                if (auto status = projects_.openProject(entries_[static_cast<std::size_t>(selection_)].root)) {
                    return NavigationRequest::replace(ScreenId::TrackView);
                }
            }
            dirty_.set();
            return NavigationRequest::none();
        }
    }
    return NavigationRequest::ignored();
}

void ProjectBrowserScreen::render(display::Painter& painter) {
    // Phase 7 draws the scrolling list.
    painter.clear(display::palette::kBackground);
    dirty_.clear();
}

}  // namespace aw::ui
