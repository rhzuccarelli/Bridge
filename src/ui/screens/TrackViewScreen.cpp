#include "aw/ui/screens/TrackViewScreen.hpp"

#include "aw/display/Color.hpp"
#include "aw/display/Font.hpp"

#include <cstdio>

namespace aw::ui {

TrackViewScreen::TrackViewScreen(project::ProjectManager& projects,
                                 audio::AudioEngine& engine) noexcept
    : projects_{projects}, engine_{engine} {}

NavigationRequest TrackViewScreen::handleEvent(const events::Event& event) {
    if (const auto* level = std::get_if<events::LevelEvent>(&event)) {
        if (level->trackId > 0 && level->trackId <= levels_.size()) {
            levels_[static_cast<std::size_t>(level->trackId - 1)] = level->peak;
            dirty_.set();
        }
        return NavigationRequest::none();
    }
    if (const auto* button = std::get_if<events::ButtonEvent>(&event)) {
        if (button->id == core::ButtonId::EncoderPush &&
            button->action == core::ButtonAction::LongPressed) {
            trackBank_ = trackBank_ == 0 ? 1 : 0;
            dirty_.set();
            return NavigationRequest::none();
        }
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
                const auto track = static_cast<core::TrackId>(index + 1 + trackBank_ * 4);
                if (track <= project::kTrackCount) {
                    focusedTrack_ = track;
                }
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
    painter.clear(display::palette::kBackground);
    const auto& font = display::smallFont();
    const char* mode = mode_ == ParameterMode::Gain ? "GAIN" :
                       mode_ == ParameterMode::Pan ? "PAN" : "POS";
    painter.fillRect({0, 0, 128, 10}, display::palette::kSurface);
    const std::string_view deckName = projects_.hasProject()
                                          ? projects_.project().activeDeck().name
                                          : std::string_view{"NO DECK"};
    painter.drawText(2, 1, deckName.substr(0, 13), font, display::palette::kText);
    char status[9];
    std::snprintf(status, sizeof(status), "B%u %s", trackBank_ + 1, mode);
    painter.drawTextIn({78, 1, 48, 8}, status, font, display::palette::kText,
                       display::TextAlign::Right);
    const project::Timeline* timeline = projects_.hasProject() ? &projects_.project().timeline() : nullptr;
    for (std::size_t index = 0; index < project::kTrackCount; ++index) {
        const auto y = static_cast<std::int16_t>(11 + index * 9);
        const bool focused = focusedTrack_ == static_cast<core::TrackId>(index + 1);
        if (focused) painter.fillRect({0, y, 128, 9}, display::palette::kSurface);
        char label[5];
        std::snprintf(label, sizeof(label), "%c%zu", focused ? '>' : ' ', index + 1);
        painter.drawText(1, y, label, font, display::palette::kText);
        if (timeline != nullptr && index < timeline->tracks.size()) {
            const project::Track& track = timeline->tracks[index];
            std::string_view name = track.name.size() > 10 ? std::string_view{track.name}.substr(0, 10)
                                                           : std::string_view{track.name};
            painter.drawText(15, y, name, font, display::palette::kText);
            if (track.muted) painter.drawText(79, y, "M", font, display::palette::kTextDim);
            if (track.soloed) painter.drawText(86, y, "S", font, display::palette::kText);
        }
        painter.drawMeter({96, static_cast<std::int16_t>(y + 2), 30, 5}, levels_[index], false);
    }
    dirty_.clear();
}

}  // namespace aw::ui
