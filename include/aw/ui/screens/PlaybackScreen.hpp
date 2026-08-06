#pragma once

#include "aw/audio/AudioEngine.hpp"
#include "aw/project/ProjectManager.hpp"
#include "aw/ui/DirtyFlag.hpp"
#include "aw/ui/Screen.hpp"

namespace aw::ui {

/// Large transport readout: timecode, full-width waveform overview, stereo meters.
///
/// Deliberately a *different screen* from TrackView rather than a mode of it.
/// While playing back a take, the useful information is position and level at a
/// glance from a distance; while editing, it is four lanes of detail.  One
/// screen trying to be both would be worse at each.
class PlaybackScreen final : public Screen {
public:
    PlaybackScreen(project::ProjectManager& projects, audio::AudioEngine& engine) noexcept;

    [[nodiscard]] ScreenId id() const noexcept override { return ScreenId::Playback; }
    [[nodiscard]] std::string_view title() const noexcept override { return "Playback"; }

    [[nodiscard]] NavigationRequest handleEvent(const events::Event& event) override;
    void update(std::uint32_t elapsedMillis) override;
    [[nodiscard]] bool isDirty() const noexcept override { return dirty_.isSet(); }
    void render(display::Painter& painter) override;

private:
    project::ProjectManager& projects_;
    audio::AudioEngine& engine_;
    DirtyFlag dirty_;
    core::FrameIndex lastDrawnPosition_{-1};
};

}  // namespace aw::ui
