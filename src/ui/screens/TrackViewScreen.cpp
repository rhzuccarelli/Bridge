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
        if (button->id == core::ButtonId::EncoderPush) {
            if (button->action == core::ButtonAction::Pressed) {
                mode_ = mode_ == ParameterMode::Gain      ? ParameterMode::Pan
                        : mode_ == ParameterMode::Pan     ? ParameterMode::Position
                                                          : ParameterMode::Gain;
                dirty_.set();
                return NavigationRequest::none();
            }
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
            case core::ButtonId::Track4:
            case core::ButtonId::Track5:
            case core::ButtonId::Track6: {
                const auto index = static_cast<std::uint8_t>(button->id) -
                                   static_cast<std::uint8_t>(core::ButtonId::Track1);
                focusedTrack_ = static_cast<core::TrackId>(index + 1);
                dirty_.set();
                return NavigationRequest::none();
            }
            case core::ButtonId::EncoderPush:
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
    painter.fillRect({0, 0, 128, 12}, display::palette::kSurface);
    const std::string_view deckName = projects_.hasProject()
                                          ? projects_.project().activeDeck().name
                                          : std::string_view{"NO DECK"};
    painter.drawText(2, 0, deckName.substr(0, 9), font, display::palette::kText);
    painter.drawTextIn({88, 0, 38, 12}, mode, font, display::palette::kText,
                       display::TextAlign::Right);
    const project::Timeline* timeline = projects_.hasProject() ? &projects_.project().timeline() : nullptr;
    const std::size_t first = focusedTrack_ > 4 ? 2U : 0U;
    for (std::size_t row = 0; row < 4 && first + row < project::kTrackCount; ++row) {
        const std::size_t index = first + row;
        const auto y = static_cast<std::int16_t>(14 + row * 12);
        const bool focused = focusedTrack_ == static_cast<core::TrackId>(index + 1);
        if (focused) painter.drawRect({0, static_cast<std::int16_t>(y - 1), 128, 12}, display::palette::kAccent);
        char label[5];
        std::snprintf(label, sizeof(label), "%c%zu", focused ? '>' : ' ', index + 1);
        painter.drawText(1, y, label, font, display::palette::kText);
        if (timeline != nullptr && index < timeline->tracks.size()) {
            const project::Track& track = timeline->tracks[index];
            std::string_view name = track.name.size() > 6 ? std::string_view{track.name}.substr(0, 6)
                                                           : std::string_view{track.name};
            painter.drawText(15, y, name, font, display::palette::kText);
            if (track.muted) painter.drawText(72, y, "M", font, display::palette::kTextDim);
            if (track.soloed) painter.drawText(80, y, "S", font, display::palette::kText);
        }
        painter.drawMeter({90, static_cast<std::int16_t>(y + 2), 36, 7}, levels_[index], false);
    }
    dirty_.clear();
}

}  // namespace aw::ui
