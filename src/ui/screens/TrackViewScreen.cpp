#include "aw/ui/screens/TrackViewScreen.hpp"

#include "aw/display/Color.hpp"
#include "aw/display/Font.hpp"
#include "aw/project/Command.hpp"

#include <cstdio>
#include <algorithm>
#include <memory>

namespace {

class ToggleMuteCommand final : public aw::project::Command {
public:
    explicit ToggleMuteCommand(aw::core::TrackId id) : id_{id} {}
    std::string_view description() const noexcept override { return "Toggle mute"; }
    aw::core::Status apply(aw::project::Project& project) override {
        aw::project::Track* track = project.mutableTimeline().findTrack(id_);
        if (!track) return aw::core::makeError(aw::core::ErrorCode::NotFound, "track");
        previous_ = track->muted;
        track->muted = !track->muted;
        return aw::core::success();
    }
    aw::core::Status revert(aw::project::Project& project) override {
        aw::project::Track* track = project.mutableTimeline().findTrack(id_);
        if (!track) return aw::core::makeError(aw::core::ErrorCode::NotFound, "track");
        track->muted = previous_;
        return aw::core::success();
    }
    bool requiresSceneRebuild() const noexcept override { return false; }
private:
    aw::core::TrackId id_;
    bool previous_{false};
};

}  // namespace

namespace aw::ui {

TrackViewScreen::TrackViewScreen(project::ProjectManager& projects,
                                 audio::AudioEngine& engine) noexcept
    : projects_{projects}, engine_{engine} {}

NavigationRequest TrackViewScreen::handleEvent(const events::Event& event) {
    if (const auto* level = std::get_if<events::LevelEvent>(&event)) {
        if (projects_.hasProject()) {
            const auto& tracks = projects_.project().timeline().tracks;
            for (std::size_t index = 0; index < tracks.size() && index < levels_.size(); ++index) {
                if (tracks[index].id == level->trackId) {
                    levels_[index] = level->peak;
                    dirty_.set();
                    break;
                }
            }
        }
        return NavigationRequest::none();
    }
    if (const auto* button = std::get_if<events::ButtonEvent>(&event)) {
        if (button->id == core::ButtonId::EncoderPush) {
            if (button->action != core::ButtonAction::Pressed)
                return NavigationRequest::ignored();
            const core::TrackId id = focusedTrack();
            if (id == core::kInvalidTrackId) return NavigationRequest::none();
            const auto& track = projects_.project().timeline().tracks[focusedIndex_];
            const bool muted = !track.muted;
            if (projects_.execute(std::make_unique<ToggleMuteCommand>(id)))
                engine_.setTrackMuted(id, muted);
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
            case core::ButtonId::Track4:
            case core::ButtonId::Track5:
            case core::ButtonId::Track6: {
                const auto index = static_cast<std::uint8_t>(button->id) -
                                   static_cast<std::uint8_t>(core::ButtonId::Track1);
                focusedIndex_ = index;
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
        int selected = std::clamp<int>(static_cast<int>(focusedIndex_) + encoder->delta, 0,
                                   static_cast<int>(project::kTrackCount - 1));
        focusedIndex_ = static_cast<std::size_t>(selected);
        dirty_.set();
        return NavigationRequest::none();
    }

    return NavigationRequest::ignored();
}

core::TrackId TrackViewScreen::focusedTrack() const noexcept {
    if (!projects_.hasProject()) return core::kInvalidTrackId;
    const auto& tracks = projects_.project().timeline().tracks;
    return focusedIndex_ < tracks.size() ? tracks[focusedIndex_].id : core::kInvalidTrackId;
}

void TrackViewScreen::update(std::uint32_t /*elapsedMillis*/) {
    // Meters and playhead move whenever the transport is not stopped.
    dirty_.update(engine_.transport().state() != events::TransportState::Stopped);
}

void TrackViewScreen::render(display::Painter& painter) {
    painter.clear(display::palette::kBackground);
    const auto& font = display::smallFont();
    const char* state = engine_.transport().state() == events::TransportState::Playing
                            ? "PLAY" : "STOP";
    painter.fillRect({0, 0, 128, 12}, display::palette::kSurface);
    const std::string_view deckName = projects_.hasProject()
                                          ? projects_.project().activeDeck().name
                                          : std::string_view{"NO DECK"};
    painter.drawText(2, 0, deckName.substr(0, 9), font, display::palette::kText);
    painter.drawTextIn({88, 0, 38, 12}, state, font, display::palette::kText,
                       display::TextAlign::Right);
    const project::Timeline* timeline = projects_.hasProject() ? &projects_.project().timeline() : nullptr;
    const std::size_t first = focusedIndex_ > 3 ? 2U : 0U;
    for (std::size_t row = 0; row < 4 && first + row < project::kTrackCount; ++row) {
        const std::size_t index = first + row;
        const auto y = static_cast<std::int16_t>(14 + row * 12);
        const bool focused = focusedIndex_ == index;
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
