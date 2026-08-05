#include "aw/project/Timeline.hpp"

#include <algorithm>

namespace aw::project {

core::FrameCount Timeline::lengthFrames() const noexcept {
    core::FrameCount longest = 0;
    for (const Track& track : tracks) {
        longest = std::max(longest, track.lengthFrames());
    }
    return longest;
}

const Track* Timeline::findTrack(core::TrackId id) const noexcept {
    const auto it = std::find_if(tracks.begin(), tracks.end(),
                                 [id](const Track& track) { return track.id == id; });
    return it == tracks.end() ? nullptr : &*it;
}

Track* Timeline::findTrack(core::TrackId id) noexcept {
    const auto it = std::find_if(tracks.begin(), tracks.end(),
                                 [id](const Track& track) { return track.id == id; });
    return it == tracks.end() ? nullptr : &*it;
}

bool Timeline::hasSolo() const noexcept {
    return std::any_of(tracks.begin(), tracks.end(),
                       [](const Track& track) { return track.soloed; });
}

}  // namespace aw::project
