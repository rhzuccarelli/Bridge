#pragma once

#include <string>

#include "aw/core/Types.hpp"

namespace aw::project {

/// A region of a source file placed on a track.
///
/// This type is the heart of non-destructive editing.  A trim moves
/// `sourceRange.start`; a split makes two clips over the same file; a delete
/// removes a clip.  None of those touch a single byte of audio.  The WAV files
/// under `audio/` are written once, at import, and are read-only from then on.
///
/// Rejected alternative: edit the audio files in place and keep an undo copy.
/// That is simpler to write and much worse to live with - every edit costs a
/// full file rewrite on an SD card, undo costs the same again, and a power cut
/// mid-edit corrupts the take.
struct Clip {
    core::ClipId id{core::kInvalidClipId};

    /// Name of the file inside the project's `audio/` directory.  A relative
    /// leaf name, never an absolute path, so a project stays movable between
    /// cards and between devices.
    std::string sourceFile;

    /// Which part of the source file this clip plays.
    core::FrameRange sourceRange{};

    /// Where the clip starts on the timeline.
    core::FrameIndex timelinePosition{0};

    /// Per-clip trim, applied on top of the track gain.  Linear.
    float gain{1.0F};

    /// Fade lengths in frames, applied at the clip edges.  Zero means a hard cut.
    core::FrameCount fadeInFrames{0};
    core::FrameCount fadeOutFrames{0};

    /// First interleaved channel in the source WAV (zero based).
    /// TP-7 pairs therefore map as 0/1, 2/3, ... 10/11.
    core::ChannelCount sourceChannel{0};

    /// One for a mono lane, two for a stereo pair.
    core::ChannelCount sourceChannelCount{2};

    /// First channel in the destination stereo track (0 = L, 1 = R).
    /// A count of two is the normal stereo route. A count of one permits an
    /// individual source channel to replace only one side of a track.
    core::ChannelCount destinationChannel{0};
    core::ChannelCount destinationChannelCount{2};

    [[nodiscard]] core::FrameCount length() const noexcept { return sourceRange.length; }
    [[nodiscard]] core::FrameIndex timelineEnd() const noexcept {
        return timelinePosition + sourceRange.length;
    }
};

}  // namespace aw::project
