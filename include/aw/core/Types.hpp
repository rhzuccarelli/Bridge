#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace aw::core {

/// Samples per second, per channel.
using SampleRate = std::uint32_t;

/// A frame is one sample for every channel.  All timeline arithmetic in the
/// project is done in frames, never in seconds: floating point seconds
/// accumulate rounding error across edits, and frames are what both the WAV
/// files and the audio callback actually deal in.
using FrameCount = std::int64_t;

/// Position on a timeline, in frames from its start.  Signed on purpose so
/// that "before zero" is representable during drag/trim maths instead of
/// wrapping to a huge positive value.
using FrameIndex = std::int64_t;

using ChannelCount = std::uint8_t;

/// Stable identifiers used inside a project.  Never reused within a project's
/// lifetime, so an undo record can always name what it refers to.
using TrackId = std::uint32_t;
using ClipId = std::uint32_t;

inline constexpr TrackId kInvalidTrackId = 0;
inline constexpr ClipId kInvalidClipId = 0;

/// Filesystem path type.
///
/// Aliased rather than used directly so the ESP32 port has a single place to
/// substitute a lighter type if `std::filesystem::path` turns out to be too
/// heavy there.  Business logic must use `core::Path` and must not call
/// `std::filesystem` free functions - all filesystem *actions* go through
/// `hal::StorageInterface`.
using Path = std::filesystem::path;

/// Half open frame range `[start, start + length)`.
struct FrameRange {
    FrameIndex start{0};
    FrameCount length{0};

    [[nodiscard]] constexpr FrameIndex end() const noexcept { return start + length; }
    [[nodiscard]] constexpr bool isEmpty() const noexcept { return length <= 0; }
    [[nodiscard]] constexpr bool contains(FrameIndex position) const noexcept {
        return position >= start && position < end();
    }
};

[[nodiscard]] constexpr bool operator==(const FrameRange& a, const FrameRange& b) noexcept {
    return a.start == b.start && a.length == b.length;
}

/// Frames <-> milliseconds helpers.  Only for display and for user facing
/// jog/scrub steps; never for edit maths.
[[nodiscard]] constexpr std::int64_t framesToMillis(FrameCount frames, SampleRate rate) noexcept {
    return rate == 0 ? 0 : (frames * 1000) / static_cast<std::int64_t>(rate);
}

[[nodiscard]] constexpr FrameCount millisToFrames(std::int64_t millis, SampleRate rate) noexcept {
    return (millis * static_cast<std::int64_t>(rate)) / 1000;
}

}  // namespace aw::core
