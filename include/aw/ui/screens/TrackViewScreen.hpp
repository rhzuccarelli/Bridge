#pragma once

#include <array>

#include "aw/audio/AudioEngine.hpp"
#include "aw/project/ProjectManager.hpp"
#include "aw/ui/DirtyFlag.hpp"
#include "aw/ui/Screen.hpp"

namespace aw::ui {

/// The main editing view: six synchronized stereo tracks with waveform overviews and
/// meters. Physical-button banking is finalized in Phase 7; this screen
/// consumes the resulting focused-track events. The encoder adjusts the
/// current parameter mode (gain / pan / position), and encoder push cycles it.
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
    [[nodiscard]] std::uint8_t trackBank() const noexcept { return trackBank_; }

private:
    project::ProjectManager& projects_;
    audio::AudioEngine& engine_;
    DirtyFlag dirty_;
    core::TrackId focusedTrack_{core::kInvalidTrackId};
    ParameterMode mode_{ParameterMode::Gain};
    std::uint8_t trackBank_{0};
    std::array<float, project::kTrackCount> levels_{};
};

}  // namespace aw::ui
