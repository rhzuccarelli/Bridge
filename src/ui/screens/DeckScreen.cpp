#include "aw/ui/screens/DeckScreen.hpp"

#include "aw/display/Color.hpp"
#include "aw/display/Font.hpp"
#include "aw/project/Command.hpp"

#include <algorithm>
#include <memory>

namespace aw::ui {
namespace {

class SelectDeckCommand final : public project::Command {
public:
    explicit SelectDeckCommand(project::DeckId selected) : selected_{selected} {}
    std::string_view description() const noexcept override { return "Change deck"; }
    core::Status apply(project::Project& project) override {
        previous_ = project.activeDeckId(); return project.setActiveDeck(selected_);
    }
    core::Status revert(project::Project& project) override { return project.setActiveDeck(previous_); }
private:
    project::DeckId selected_;
    project::DeckId previous_{project::kInvalidDeckId};
};

}  // namespace

void DeckScreen::onEnter() {
    selection_ = 0;
    if (projects_.hasProject()) {
        const auto& decks = projects_.project().decks();
        for (std::size_t i = 0; i < decks.size(); ++i)
            if (decks[i].id == projects_.project().activeDeckId()) selection_ = static_cast<int>(i);
    }
    dirty_.set();
}

NavigationRequest DeckScreen::handleEvent(const events::Event& event) {
    if (const auto* encoder = std::get_if<events::EncoderEvent>(&event)) {
        const int count = projects_.hasProject() ? static_cast<int>(projects_.project().decks().size()) : 0;
        selection_ = std::clamp<int>(selection_ + encoder->delta, 0, std::max(0, count - 1));
        dirty_.set(); return NavigationRequest::none();
    }
    const auto* button = std::get_if<events::ButtonEvent>(&event);
    if (!button || button->action != core::ButtonAction::Pressed ||
        button->id != core::ButtonId::EncoderPush) return NavigationRequest::ignored();
    if (!projects_.hasProject() || selection_ >= static_cast<int>(projects_.project().decks().size()))
        return NavigationRequest::back();
    const auto id = projects_.project().decks()[static_cast<std::size_t>(selection_)].id;
    if (projects_.execute(std::make_unique<SelectDeckCommand>(id))) return NavigationRequest::back();
    return NavigationRequest::none();
}

void DeckScreen::render(display::Painter& painter) {
    painter.clear(display::palette::kBackground);
    const auto& font = display::smallFont();
    painter.fillRect({0, 0, 128, 12}, display::palette::kSurface);
    painter.drawText(2, 0, "ACTIVE DECK", font, display::palette::kText);
    if (projects_.hasProject()) {
        const auto& decks = projects_.project().decks();
        const int start = std::max(0, std::min(selection_ - 1, std::max(0, static_cast<int>(decks.size()) - 4)));
        for (int row = 0; row < 4 && start + row < static_cast<int>(decks.size()); ++row) {
            const int index = start + row;
            const auto y = static_cast<std::int16_t>(14 + row * 12);
            if (index == selection_) painter.drawRect({0, static_cast<std::int16_t>(y - 1), 128, 12}, display::palette::kAccent);
            painter.drawText(3, y, decks[static_cast<std::size_t>(index)].name.substr(0, 15), font,
                             display::palette::kText);
        }
    }
    dirty_.clear();
}

}  // namespace aw::ui
