#pragma once

#include <vector>

#include "aw/project/ProjectManager.hpp"
#include "aw/ui/DirtyFlag.hpp"
#include "aw/ui/Screen.hpp"

namespace aw::ui {

/// Scrolling list of projects on the card, with "New project" at the top.
///
/// Encoder scrolls, encoder push opens.  No confirmation to open, because
/// opening is not destructive and an extra press on a device driven by one knob
/// is a real cost.
class ProjectBrowserScreen final : public Screen {
public:
    explicit ProjectBrowserScreen(project::ProjectManager& projects) noexcept;

    [[nodiscard]] ScreenId id() const noexcept override { return ScreenId::ProjectBrowser; }
    [[nodiscard]] std::string_view title() const noexcept override { return "Projects"; }

    void onEnter() override;
    [[nodiscard]] NavigationRequest handleEvent(const events::Event& event) override;
    [[nodiscard]] bool isDirty() const noexcept override { return dirty_.isSet(); }
    void render(display::Painter& painter) override;

private:
    project::ProjectManager& projects_;
    std::vector<project::ProjectSummary> entries_;
    DirtyFlag dirty_;
    int selection_{0};
    int scrollOffset_{0};
};

}  // namespace aw::ui
