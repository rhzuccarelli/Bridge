#include "aw/ui/screens/HomeScreen.hpp"

#include "aw/display/Color.hpp"

namespace aw::ui {

namespace {
/// The home actions, in encoder order.  Four entries, so the whole hub is one
/// short list rather than a menu.
constexpr int kActionCount = 4;
}  // namespace

HomeScreen::HomeScreen(project::ProjectManager& projects, audio::AudioEngine& engine) noexcept
    : projects_{projects}, engine_{engine} {}

std::string_view HomeScreen::title() const noexcept {
    return projects_.hasProject() ? projects_.project().name() : std::string_view{"No project"};
}

NavigationRequest HomeScreen::handleEvent(const events::Event& event) {
    if (const auto* encoder = std::get_if<events::EncoderEvent>(&event)) {
        const int next = selection_ + encoder->delta;
        selection_ = next < 0 ? 0 : (next >= kActionCount ? kActionCount - 1 : next);
        dirty_.set();
        return NavigationRequest::none();
    }

    if (const auto* button = std::get_if<events::ButtonEvent>(&event)) {
        if (button->action != core::ButtonAction::Pressed) {
            return NavigationRequest::ignored();
        }
        if (button->id == core::ButtonId::EncoderPush) {
            switch (selection_) {
                case 0: return NavigationRequest::push(ScreenId::ProjectBrowser);
                case 1: return NavigationRequest::push(ScreenId::TrackView);
                case 2: return NavigationRequest::push(ScreenId::Playback);
                default: return NavigationRequest::push(ScreenId::Settings);
            }
        }
    }

    // Anything else - transport, Import, Export - is a global shortcut.
    return NavigationRequest::ignored();
}

void HomeScreen::update(std::uint32_t /*elapsedMillis*/) {
    // Redraw when the transport moves, so the state badge stays honest.
    dirty_.update(engine_.transport().state() != events::TransportState::Stopped);
}

void HomeScreen::render(display::Painter& painter) {
    // Phase 7 draws the real layout: project name, transport badge, device
    // status, and the four actions.
    painter.clear(display::palette::kBackground);
    dirty_.clear();
}

}  // namespace aw::ui
