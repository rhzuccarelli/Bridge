#pragma once

#include "aw/audio/AudioEngine.hpp"
#include "aw/project/ProjectManager.hpp"
#include "aw/ui/DirtyFlag.hpp"
#include "aw/ui/Screen.hpp"

namespace aw::ui {

/// The main editing view: four track lanes with waveform overviews and meters.
///
/// The four physical track buttons select the focused track directly - no
/// scrolling to reach a track, ever.  The encoder then adjusts whatever the
/// current parameter mode is (gain / pan / position), and encoder push cycles
/// that mode.  One knob, one button, no menu: this is the interaction pattern
/// the whole UI is built on.
class TrackViewScreen final : public Screen {
public:
    /// What the encoder currently edits on the focused track.
    enum class ParameterMode : std::uint8_t { Gain, Pan, Position };

    TrackViewScreen(project::ProjectManager& projects, audio::AudioEngine& engine) noexcept;

    [[nodiscard]] ScreenId id() const noexcept override { return ScreenId::TrackView; }
    [[nodiscard]] std::string_view title() const noexcept override { return "Tracks"; }

    [[nodiscard]] NavigationRequest handleEvent(const events::Event& event) override;
    void update(std::uint32_t elapsedMillis) override;
    [[nodiscard]] bool isDirty() const noexcept override { return dirty_.isSet(); }
    void render(display::Painter& painter) override;

    [[nodiscard]] core::TrackId focusedTrack() const noexcept { return focusedTrack_; }
    [[nodiscard]] ParameterMode parameterMode() const noexcept { return mode_; }

private:
    project::ProjectManager& projects_;
    audio::AudioEngine& engine_;
    DirtyFlag dirty_;
    core::TrackId focusedTrack_{core::kInvalidTrackId};
    ParameterMode mode_{ParameterMode::Gain};
};

}  // namespace aw::ui
