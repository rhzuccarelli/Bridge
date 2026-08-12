#pragma once

#include <cstddef>
#include <vector>

#include "aw/core/Types.hpp"
#include "aw/project/Track.hpp"

namespace aw::project {

/// The device's fixed synchronized stereo-track count. This is a product invariant,
/// independent of how the smaller set of physical buttons is banked in the UI.
inline constexpr std::size_t kTrackCount = 6;

/// All tracks plus the arrangement-wide settings.
///
/// A plain aggregate with query helpers, not a class with behaviour: every
/// *mutation* goes through a `Command` so that undo works, which means the
/// timeline itself has nothing to protect.  Anything that mutates state here
/// without going through the command layer is a bug waiting to be undone
/// incorrectly.
struct Timeline {
    core::SampleRate sampleRate{48000};
    std::vector<Track> tracks;
    core::FrameRange loopRange{};
    bool loopEnabled{false};

    [[nodiscard]] core::FrameCount lengthFrames() const noexcept;

    [[nodiscard]] const Track* findTrack(core::TrackId id) const noexcept;
    [[nodiscard]] Track* findTrack(core::TrackId id) noexcept;

    /// True when at least one track is soloed - the mixer's solo rule needs it.
    [[nodiscard]] bool hasSolo() const noexcept;
};

}  // namespace aw::project
