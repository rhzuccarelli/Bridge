#pragma once

#include <string>
#include <vector>

#include "aw/core/Types.hpp"
#include "aw/project/Clip.hpp"

namespace aw::project {

/// One track of the multitrack timeline.
///
/// Four tracks is the device's fixed configuration (four track buttons on the
/// front panel), but nothing here hard-codes that number - the constant lives
/// in `Timeline`, so a future eight-track revision is a constant change rather
/// than a data model change.
struct Track {
    core::TrackId id{core::kInvalidTrackId};
    std::string name;                 ///< "Track01" by default; user renameable.
    std::vector<Clip> clips;          ///< Ordered by `timelinePosition`.

    float gain{1.0F};                 ///< Linear.
    float pan{0.0F};                  ///< -1 left .. +1 right.
    bool muted{false};
    bool soloed{false};
    bool recordArmed{false};          ///< Reserved for punch-in (Phase 4+).

    [[nodiscard]] bool isEmpty() const noexcept { return clips.empty(); }

    /// Frame after the last clip's end, or 0 for an empty track.
    [[nodiscard]] core::FrameCount lengthFrames() const noexcept;

    [[nodiscard]] const Clip* clipAt(core::FrameIndex position) const noexcept;
};

}  // namespace aw::project
