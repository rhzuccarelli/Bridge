#include "aw/ui/screens/HomeScreen.hpp"

#include "aw/display/Color.hpp"
#include "aw/display/Font.hpp"

#include <array>

namespace aw::ui {

namespace {
/// The home actions, in encoder order.  Four entries, so the whole hub is one
/// short list rather than a menu.
constexpr int kActionCount = 7;
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
        if (selection_ < scrollOffset_) scrollOffset_ = selection_;
        if (selection_ >= scrollOffset_ + 4) scrollOffset_ = selection_ - 3;
        dirty_.set();
        return NavigationRequest::none();
    }

    if (const auto* button = std::get_if<events::ButtonEvent>(&event)) {
        if (button->action != core::ButtonAction::Pressed) {
            return NavigationRequest::ignored();
        }
        if (button->id == core::ButtonId::EncoderPush) {
            switch (selection_) {
                case 0:
                    return NavigationRequest::push(ScreenId::Playback);
                case 1: return NavigationRequest::push(ScreenId::TrackView);
                case 2: return NavigationRequest::push(ScreenId::CopyChannel);
                case 3: return NavigationRequest::push(ScreenId::Decks);
                case 4: return NavigationRequest::push(ScreenId::Import);
                case 5: return NavigationRequest::push(ScreenId::Export);
                default: return NavigationRequest::push(ScreenId::ProjectBrowser);
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
    painter.clear(display::palette::kBackground);
    const auto& font = display::smallFont();
    painter.fillRect({0, 0, 128, 12}, display::palette::kSurface);
    painter.drawTextIn({2, 0, 86, 12}, title().substr(0, 10), font, display::palette::kText,
                       display::TextAlign::Left);
    const char* state = engine_.transport().state() == events::TransportState::Playing ? "PLAY" :
                        engine_.transport().state() == events::TransportState::Recording ? "REC" :
                        engine_.transport().state() == events::TransportState::Paused ? "PAUS" : "STOP";
    painter.drawTextIn({90, 0, 36, 12}, state, font, display::palette::kText,
                       display::TextAlign::Right);
    constexpr std::array<std::string_view, 7> actions{
        "TRANSPORT", "TRACKS", "COPY CHANNEL", "CHANGE DECK",
        "IMPORT TP-7", "EXPORT TP-7", "PROJECTS"};
    for (int row = 0; row < 4; ++row) {
        const int index = scrollOffset_ + row;
        if (index >= kActionCount) break;
        const std::int16_t y = static_cast<std::int16_t>(14 + row * 12);
        if (index == selection_) painter.drawRect(
            {1, static_cast<std::int16_t>(y - 1), 126, 12}, display::palette::kAccent);
        painter.drawText(5, y, index == selection_ ? ">" : " ", font, display::palette::kText);
        painter.drawText(14, y, actions[static_cast<std::size_t>(index)], font,
                         display::palette::kText);
    }
    dirty_.clear();
}

}  // namespace aw::ui
