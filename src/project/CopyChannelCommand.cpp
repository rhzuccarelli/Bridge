#include "aw/project/CopyChannelCommand.hpp"

#include "aw/project/Project.hpp"

#include <algorithm>

namespace aw::project {
namespace {

bool affectsSide(const Clip& clip, core::ChannelCount side) noexcept {
    return side >= clip.destinationChannel &&
           side < clip.destinationChannel + clip.destinationChannelCount;
}

Clip isolateSide(Clip clip, core::ChannelCount side) noexcept {
    const core::ChannelCount routeOffset = side - clip.destinationChannel;
    clip.sourceChannel += std::min(routeOffset, clip.sourceChannelCount - 1U);
    clip.sourceChannelCount = 1;
    clip.destinationChannel = side;
    clip.destinationChannelCount = 1;
    return clip;
}

}  // namespace

CopyChannelCommand::CopyChannelCommand(DeckId sourceDeck, std::size_t sourceTrack,
                                       core::ChannelCount sourceSide,
                                       DeckId destinationDeck, std::size_t destinationTrack,
                                       core::ChannelCount destinationSide) noexcept
    : sourceDeck_{sourceDeck}, sourceTrack_{sourceTrack}, sourceSide_{sourceSide},
      destinationDeck_{destinationDeck}, destinationTrack_{destinationTrack},
      destinationSide_{destinationSide} {}

core::Status CopyChannelCommand::apply(Project& project) {
    if (sourceSide_ > 1 || destinationSide_ > 1) {
        return core::makeError(core::ErrorCode::InvalidArgument, "channel side must be L or R");
    }
    const Deck* sourceDeck = project.findDeck(sourceDeck_);
    Deck* destinationDeck = project.findDeck(destinationDeck_);
    if (!sourceDeck || !destinationDeck) {
        return core::makeError(core::ErrorCode::NotFound, "deck");
    }
    if (sourceTrack_ >= sourceDeck->timeline.tracks.size() ||
        destinationTrack_ >= destinationDeck->timeline.tracks.size()) {
        return core::makeError(core::ErrorCode::NotFound, "track");
    }

    if (!prepared_) {
        previousActiveDeck_ = project.activeDeckId();
        previousClips_ = destinationDeck->timeline.tracks[destinationTrack_].clips;
        const std::vector<Clip> sourceClips = sourceDeck->timeline.tracks[sourceTrack_].clips;

        for (const Clip& original : previousClips_) {
            if (!affectsSide(original, destinationSide_)) {
                routedClips_.push_back(original);
            } else if (original.destinationChannelCount == 2) {
                routedClips_.push_back(isolateSide(original, 1U - destinationSide_));
            }
        }
        for (const Clip& original : sourceClips) {
            if (!affectsSide(original, sourceSide_)) continue;
            Clip routed = isolateSide(original, sourceSide_);
            routed.id = project.allocateClipId();
            routed.destinationChannel = destinationSide_;
            routedClips_.push_back(std::move(routed));
        }
        std::stable_sort(routedClips_.begin(), routedClips_.end(),
                         [](const Clip& a, const Clip& b) {
                             return a.timelinePosition < b.timelinePosition;
                         });
        prepared_ = true;
    }

    destinationDeck = project.findDeck(destinationDeck_);
    destinationDeck->timeline.tracks[destinationTrack_].clips = routedClips_;
    if (auto status = project.setActiveDeck(destinationDeck_); !status) return status;
    applied_ = true;
    return core::success();
}

core::Status CopyChannelCommand::revert(Project& project) {
    if (!applied_) {
        return core::makeError(core::ErrorCode::InvalidState, "copy channel is not applied");
    }
    Deck* destinationDeck = project.findDeck(destinationDeck_);
    if (!destinationDeck || destinationTrack_ >= destinationDeck->timeline.tracks.size()) {
        return core::makeError(core::ErrorCode::NotFound, "destination track");
    }
    destinationDeck->timeline.tracks[destinationTrack_].clips = previousClips_;
    if (auto status = project.setActiveDeck(previousActiveDeck_); !status) return status;
    applied_ = false;
    return core::success();
}

}  // namespace aw::project
