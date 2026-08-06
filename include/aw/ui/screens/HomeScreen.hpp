#pragma once

#include "aw/audio/AudioEngine.hpp"
#include "aw/project/ProjectManager.hpp"
#include "aw/ui/DirtyFlag.hpp"
#include "aw/ui/Screen.hpp"

namespace aw::ui {

/// The screen the device boots into.
///
/// Shows the open project, the transport state and the device connection
/// status, and is the hub every other screen is one press away from.  It exists
/// so the answer to "where am I" is always at most one Back press, which is what
/// replaces the menu tree a device with no keyboard cannot afford.
class HomeScreen final : public Screen {
public:
    HomeScreen(project::ProjectManager& projects, audio::AudioEngine& engine) noexcept;

    [[nodiscard]] ScreenId id() const noexcept override { return ScreenId::Home; }
    [[nodiscard]] std::string_view title() const noexcept override;

    [[nodiscard]] NavigationRequest handleEvent(const events::Event& event) override;
    void update(std::uint32_t elapsedMillis) override;
    [[nodiscard]] bool isDirty() const noexcept override { return dirty_.isSet(); }
    void render(display::Painter& painter) override;

private:
    project::ProjectManager& projects_;
    audio::AudioEngine& engine_;
    DirtyFlag dirty_;
    int selection_{0};  ///< Encoder position over the home actions.
};

}  // namespace aw::ui
