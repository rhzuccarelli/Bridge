#pragma once

#include "aw/project/ProjectManager.hpp"
#include "aw/ui/DirtyFlag.hpp"
#include "aw/ui/Screen.hpp"

namespace aw::ui {

/// Encoder wizard for cross-deck, side-specific channel references.
class CopyChannelScreen final : public Screen {
public:
    explicit CopyChannelScreen(project::ProjectManager& projects) noexcept : projects_{projects} {}

    [[nodiscard]] ScreenId id() const noexcept override { return ScreenId::CopyChannel; }
    [[nodiscard]] std::string_view title() const noexcept override { return "Copy channel"; }
    void onEnter() override;
    [[nodiscard]] NavigationRequest handleEvent(const events::Event& event) override;
    [[nodiscard]] bool isDirty() const noexcept override { return dirty_.isSet(); }
    void render(display::Painter& painter) override;

private:
    static constexpr std::size_t kStepCount = 7;
    void change(int delta);

    project::ProjectManager& projects_;
    DirtyFlag dirty_;
    std::size_t step_{0};
    std::size_t sourceDeck_{0};
    std::size_t destinationDeck_{0};
    std::size_t sourceTrack_{0};
    std::size_t destinationTrack_{0};
    core::ChannelCount sourceSide_{0};
    core::ChannelCount destinationSide_{0};
    core::ErrorCode error_{core::ErrorCode::Unknown};
};

}  // namespace aw::ui
