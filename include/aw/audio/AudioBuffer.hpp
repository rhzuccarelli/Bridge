#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "aw/core/Types.hpp"

namespace aw::audio {

/// Fixed capacity, non-interleaved float buffer.
///
/// Planar rather than interleaved because every per-track operation the mixer
/// performs (gain, mute, peak) is a straight run over one channel, and planar
/// layout keeps that a simple contiguous loop that vectorises.  Interleaving
/// happens once, at the very end, on the way to the output callback.
///
/// Memory is allocated exactly once, in the constructor.  `setFrameCount` only
/// shrinks the *active* size within the reserved capacity, so nothing on the
/// realtime path ever touches the allocator.
class AudioBuffer {
public:
    AudioBuffer() = default;
    AudioBuffer(core::ChannelCount channels, core::FrameCount capacityFrames);

    [[nodiscard]] core::ChannelCount channelCount() const noexcept { return channels_; }
    [[nodiscard]] core::FrameCount frameCount() const noexcept { return frames_; }
    [[nodiscard]] core::FrameCount capacityFrames() const noexcept { return capacity_; }

    /// Precondition: `frames <= capacityFrames()`.  Realtime safe.
    void setFrameCount(core::FrameCount frames) noexcept;

    /// Precondition: `channel < channelCount()`.
    [[nodiscard]] std::span<float> channel(core::ChannelCount channel) noexcept;
    [[nodiscard]] std::span<const float> channel(core::ChannelCount channel) const noexcept;

    /// Zeroes the active region.  Realtime safe.
    void clear() noexcept;

private:
    core::ChannelCount channels_{0};
    core::FrameCount capacity_{0};
    core::FrameCount frames_{0};
    std::vector<float> samples_;  ///< channels_ * capacity_, channel major.
};

}  // namespace aw::audio
