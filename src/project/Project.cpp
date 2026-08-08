#include "aw/project/Project.hpp"

#include <algorithm>

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

core::Status Project::removeDeck(DeckId id) noexcept {
    if (decks_.size() <= 1) {
        return core::makeError(core::ErrorCode::InvalidState, "cannot remove the last deck");
    }
    const auto it = std::find_if(decks_.begin(), decks_.end(),
                                 [id](const Deck& deck) { return deck.id == id; });
    if (it == decks_.end()) return core::makeError(core::ErrorCode::NotFound, "deck");
    const std::size_t removedIndex = static_cast<std::size_t>(it - decks_.begin());
    decks_.erase(it);
    if (activeDeckIndex_ == removedIndex) activeDeckIndex_ = 0;
    else if (activeDeckIndex_ > removedIndex) --activeDeckIndex_;
    return core::success();
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
