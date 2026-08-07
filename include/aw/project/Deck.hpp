#pragma once

#include <cstdint>
#include <string>

#include "aw/project/Timeline.hpp"

namespace aw::project {

using DeckId = std::uint32_t;
inline constexpr DeckId kInvalidDeckId = 0;

/// One six-lane arrangement in a project.
///
/// Only Project::activeDeck() is rendered. Other decks remain available as
/// non-destructive source libraries while the transport is running.
struct Deck {
    DeckId id{kInvalidDeckId};
    std::string name;
    Timeline timeline;
};

}  // namespace aw::project
