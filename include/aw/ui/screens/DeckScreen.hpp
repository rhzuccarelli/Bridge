#pragma once

#include "aw/project/ProjectManager.hpp"
#include "aw/ui/DirtyFlag.hpp"
#include "aw/ui/Screen.hpp"

namespace aw::ui {

class DeckScreen final : public Screen {
public:
    explicit DeckScreen(project::ProjectManager& projects) noexcept : projects_{projects} {}
    [[nodiscard]] ScreenId id() const noexcept override { return ScreenId::Decks; }
    [[nodiscard]] std::string_view title() const noexcept override { return "Decks"; }
    void onEnter() override;
    [[nodiscard]] NavigationRequest handleEvent(const events::Event& event) override;
    [[nodiscard]] bool isDirty() const noexcept override { return dirty_.isSet(); }
    void render(display::Painter& painter) override;
private:
    project::ProjectManager& projects_;
    DirtyFlag dirty_;
    int selection_{0};
};

}  // namespace aw::ui
