#include "aw/project/Project.hpp"

namespace aw::project {

Project::Project(std::string name, core::Path root)
    : name_{std::move(name)}, paths_{std::move(root)} {
    (void)addDeck("Deck A");
}

Deck& Project::addDeck(std::string name) {
    Deck deck;
    deck.id = ++lastDeckId_;
    deck.name = std::move(name);
    deck.timeline.sampleRate = 48000;
    decks_.push_back(std::move(deck));
    return decks_.back();
}

core::Status Project::setActiveDeck(DeckId id) noexcept {
    for (std::size_t index = 0; index < decks_.size(); ++index) {
        if (decks_[index].id == id) {
            activeDeckIndex_ = index;
            return core::success();
        }
    }
    return core::makeError(core::ErrorCode::NotFound, "deck");
}

}  // namespace aw::project
