#include "aw/ui/screens/TrackViewScreen.hpp"

#include "aw/display/Color.hpp"

namespace aw::ui {

TrackViewScreen::TrackViewScreen(project::ProjectManager& projects,
                                 audio::AudioEngine& engine) noexcept
    : projects_{projects}, engine_{engine} {}

NavigationRequest TrackViewScreen::handleEvent(const events::Event& event) {
    if (const auto* button = std::get_if<events::ButtonEvent>(&event)) {
        if (button->action != core::ButtonAction::Pressed) {
            return NavigationRequest::ignored();
        }

        // Direct track selection: one press, always, from anywhere in this view.
        switch (button->id) {
            case core::ButtonId::Track1:
            case core::ButtonId::Track2:
            case core::ButtonId::Track3:
            case core::ButtonId::Track4: {
                const auto index = static_cast<std::uint8_t>(button->id) -
                                   static_cast<std::uint8_t>(core::ButtonId::Track1);
                focusedTrack_ = static_cast<core::TrackId>(index + 1);
                dirty_.set();
                return NavigationRequest::none();
            }
            case core::ButtonId::EncoderPush:
                // Cycle what the encoder edits. Three modes, no menu.
                mode_ = mode_ == ParameterMode::Gain      ? ParameterMode::Pan
                        : mode_ == ParameterMode::Pan     ? ParameterMode::Position
                                                          : ParameterMode::Gain;
                dirty_.set();
                return NavigationRequest::none();
            default:
                return NavigationRequest::ignored();
        }
    }

    if (const auto* encoder = std::get_if<events::EncoderEvent>(&event)) {
        // Phase 3/4 turns this into an undoable command (gain/pan) or a seek.
        (void)encoder;
        (void)projects_;
        dirty_.set();
        return NavigationRequest::none();
    }

    return NavigationRequest::ignored();
}

void TrackViewScreen::update(std::uint32_t /*elapsedMillis*/) {
    // Meters and playhead move whenever the transport is not stopped.
    dirty_.update(engine_.transport().state() != events::TransportState::Stopped);
}

void TrackViewScreen::render(display::Painter& painter) {
    // Phase 7 draws four lanes: name, waveform overview, meter, mute/solo state.
    painter.clear(display::palette::kBackground);
    dirty_.clear();
}

}  // namespace aw::ui
