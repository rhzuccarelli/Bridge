#include "aw/ui/screens/ProjectBrowserScreen.hpp"

#include "aw/display/Color.hpp"
#include "aw/display/Font.hpp"

#include <string>

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
        const int count = static_cast<int>(entries_.size()) + 1;
        const int next = selection_ + encoder->delta;
        selection_ = next < 0 ? 0 : (next >= count ? (count > 0 ? count - 1 : 0) : next);
        if (selection_ < scrollOffset_) scrollOffset_ = selection_;
        if (selection_ >= scrollOffset_ + 4) scrollOffset_ = selection_ - 3;
        dirty_.set();
        return NavigationRequest::none();
    }

    if (const auto* button = std::get_if<events::ButtonEvent>(&event)) {
        if (button->action == core::ButtonAction::Pressed &&
            button->id == core::ButtonId::EncoderPush) {
            if (selection_ == 0) {
                int suffix = 1;
                core::Status status = core::makeError(core::ErrorCode::AlreadyExists, "project");
                while (!status && status.error().code() == core::ErrorCode::AlreadyExists) {
                    status = projects_.createProject("Project " + std::to_string(suffix++));
                }
                if (status) return NavigationRequest::replace(ScreenId::TrackView);
            } else if (selection_ <= static_cast<int>(entries_.size())) {
                if (auto status = projects_.openProject(
                        entries_[static_cast<std::size_t>(selection_ - 1)].root)) {
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
    painter.clear(display::palette::kBackground);
    const auto& font = display::smallFont();
    painter.fillRect({0, 0, 128, 12}, display::palette::kSurface);
    painter.drawText(2, 0, "PROJECTS", font, display::palette::kText);
    for (int visible = 0; visible < 4; ++visible) {
        const int index = scrollOffset_ + visible;
        if (index > static_cast<int>(entries_.size())) break;
        const auto y = static_cast<std::int16_t>(14 + visible * 12);
        if (index == selection_) painter.drawRect(
            {0, static_cast<std::int16_t>(y - 1), 128, 12}, display::palette::kAccent);
        std::string_view name = index == 0
                                    ? std::string_view{"+ NEW PROJECT"}
                                    : std::string_view{entries_[static_cast<std::size_t>(index - 1)].name};
        if (name.size() > 15) name = name.substr(0, 15);
        painter.drawText(3, y, name, font, display::palette::kText);
    }
    dirty_.clear();
}

}  // namespace aw::ui
