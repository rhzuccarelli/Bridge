#pragma once

#include <array>

#include "aw/audio/AudioEngine.hpp"
#include "aw/project/ProjectManager.hpp"
#include "aw/ui/DirtyFlag.hpp"
#include "aw/ui/Screen.hpp"

namespace aw::ui {

/// The main editing view: six synchronized stereo tracks with waveform overviews and
/// meters. The encoder moves directly among the six lanes; there are no hidden
/// gain/pan/position modes in the one-knob interface.
class TrackViewScreen final : public Screen {
public:
    TrackViewScreen(project::ProjectManager& projects, audio::AudioEngine& engine) noexcept;

    [[nodiscard]] ScreenId id() const noexcept override { return ScreenId::TrackView; }
    [[nodiscard]] std::string_view title() const noexcept override { return "Tracks"; }

    [[nodiscard]] NavigationRequest handleEvent(const events::Event& event) override;
    void update(std::uint32_t elapsedMillis) override;
    [[nodiscard]] bool isDirty() const noexcept override { return dirty_.isSet(); }
    void render(display::Painter& painter) override;

    [[nodiscard]] core::TrackId focusedTrack() const noexcept { return focusedTrack_; }

private:
    project::ProjectManager& projects_;
    audio::AudioEngine& engine_;
    DirtyFlag dirty_;
    core::TrackId focusedTrack_{core::kInvalidTrackId};
    std::array<float, project::kTrackCount> levels_{};
};

}  // namespace aw::ui
