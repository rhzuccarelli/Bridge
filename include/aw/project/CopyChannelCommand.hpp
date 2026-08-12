#pragma once

#include <vector>

#include "aw/project/Command.hpp"
#include "aw/project/Deck.hpp"
#include "aw/project/Track.hpp"

namespace aw::project {

/// Replaces one side of a destination track with non-destructive references
/// to one side of a source track in any deck.
class CopyChannelCommand final : public Command {
public:
    CopyChannelCommand(DeckId sourceDeck, std::size_t sourceTrack,
                       core::ChannelCount sourceSide, DeckId destinationDeck,
                       std::size_t destinationTrack, core::ChannelCount destinationSide) noexcept;

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Copy channel";
    }
    [[nodiscard]] core::Status apply(Project& project) override;
    [[nodiscard]] core::Status revert(Project& project) override;

private:
    DeckId sourceDeck_;
    std::size_t sourceTrack_;
    core::ChannelCount sourceSide_;
    DeckId destinationDeck_;
    std::size_t destinationTrack_;
    core::ChannelCount destinationSide_;
    DeckId previousActiveDeck_{kInvalidDeckId};
    std::vector<Clip> previousClips_;
    std::vector<Clip> routedClips_;
    bool prepared_{false};
    bool applied_{false};
};

}  // namespace aw::project
