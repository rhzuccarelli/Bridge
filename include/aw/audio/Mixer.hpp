#pragma once

#include <span>
#include <vector>

#include "aw/audio/AudioBuffer.hpp"
#include "aw/audio/TrackPlayer.hpp"
#include "aw/core/Types.hpp"

namespace aw::audio {

/// Sums track players into the stereo bus.
///
/// Phase 4 implements this.  It owns no files and no threads: it is handed a
/// list of players and a block to fill.  Keeping it that dumb is what makes the
/// mix path testable - a test can drive it with synthetic players and assert on
/// the summed output without any hardware or disk in the picture.
///
/// Solo semantics are decided here, once: if any track is soloed, non-soloed
/// tracks are silent regardless of their mute state.  Putting that rule in one
/// place stops it drifting between the engine and the UI.
class Mixer {
public:
    Mixer() = default;

    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    /// Pre-allocates for the worst case.  Called at startup, never later.
    void prepare(core::ChannelCount outputChannels, core::FrameCount maxBlockFrames);

    /// Realtime safe.  `destination` is cleared and then filled.
    void render(std::span<TrackPlayer* const> tracks,
                AudioBuffer& destination,
                core::FrameIndex timelinePosition,
                core::FrameCount frames) noexcept;

    /// Master gain, linear.  Applied after summing, before the output.
    void setMasterGain(float gain) noexcept;
    [[nodiscard]] float masterGain() const noexcept;

    /// Peak of the last rendered block, per output channel.
    [[nodiscard]] float lastPeak(core::ChannelCount channel) const noexcept;

private:
    core::ChannelCount channels_{2};
    core::FrameCount maxBlockFrames_{0};
    float masterGain_{1.0F};
    AudioBuffer scratch_;                ///< Per-track render target, reused.
    std::vector<float> peaks_{0.0F, 0.0F};
};

}  // namespace aw::audio
