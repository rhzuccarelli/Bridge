#pragma once

#include <atomic>
#include <memory>

#include "aw/audio/AudioBuffer.hpp"
#include "aw/audio/WavReader.hpp"
#include "aw/core/Result.hpp"
#include "aw/core/Types.hpp"

namespace aw::audio {

/// Per-track mix parameters.
///
/// Plain values, updated atomically from the UI thread and read once per audio
/// block. Per-block snapshots keep both channels coherent and bound parameter
/// latency to one hardware period.
struct TrackMixState {
    std::atomic<float> gain{1.0F};    ///< Linear, 0 .. ~4 (+12 dB).
    std::atomic<float> pan{0.0F};     ///< -1 hard left .. +1 hard right.
    std::atomic<bool> muted{false};
    std::atomic<bool> soloed{false};
};

/// Routes one mono/stereo lane from a streamed source and applies mix parameters.
///
/// Phase 4 implements this.  The split that matters: `prefetch()` runs on the
/// streaming thread and does all the file I/O, filling a ring buffer shared by
/// every route over the same source placement;
/// `render()` runs on the audio thread and only ever consumes from that ring.
/// If the ring runs dry the track outputs silence and flags an underrun - it
/// never blocks, and one slow track cannot take the whole mix down.
class TrackPlayer {
public:
    TrackPlayer();
    ~TrackPlayer();

    TrackPlayer(const TrackPlayer&) = delete;
    TrackPlayer& operator=(const TrackPlayer&) = delete;

    /// Binds this player to an audio file.  Called from the UI/loader thread
    /// while the transport is stopped, or on a background load for a new clip.
    [[nodiscard]] core::Status setSource(hal::StorageInterface& storage,
                                         const core::Path& path,
                                         core::ChannelCount sourceChannel = 0,
                                         core::ChannelCount sourceChannelCount = 2,
                                         core::ChannelCount destinationChannel = 0,
                                         core::ChannelCount destinationChannelCount = 2);

    /// Adds another mix route over an already-open stream. Both players consume
    /// the same multichannel blocks; no second file handle or ring is created.
    [[nodiscard]] core::Status shareSource(const TrackPlayer& sourceOwner,
                                            core::ChannelCount sourceChannel,
                                            core::ChannelCount sourceChannelCount = 2,
                                            core::ChannelCount destinationChannel = 0,
                                            core::ChannelCount destinationChannelCount = 2);

    void clearSource() noexcept;
    [[nodiscard]] bool hasSource() const noexcept;

    [[nodiscard]] TrackMixState& mix() noexcept { return mix_; }
    [[nodiscard]] const TrackMixState& mix() const noexcept { return mix_; }

    // --- Streaming thread ---------------------------------------------------
    /// Reads this clip's source range ahead from disk so `render()` never has
    /// to wait.  Blocking.
    [[nodiscard]] core::Status prefetch(core::FrameIndex timelinePosition,
                                        core::FrameRange sourceRange = {});

    /// Refills the bounded SPSC stream. Called only by the streaming thread.
    /// Returns true if it produced at least one block.
    [[nodiscard]] bool stream();
    void setLoop(core::FrameRange range, bool enabled) noexcept;

    /// Audio-thread notification for an explicit seek (loop wraps do not call
    /// this because the producer has already queued the loop head).
    void requestStreamPosition(core::FrameIndex timelinePosition) noexcept;

    // --- Audio thread -------------------------------------------------------
    /// Adds this track's contribution into `destination` (which is *not*
    /// cleared first - the mixer owns that).  Realtime safe, never blocks.
    void render(AudioBuffer& destination,
                core::FrameIndex timelinePosition,
                core::FrameCount frames) noexcept;

    /// Peak of the last rendered block, for the meters.  Realtime safe to read.
    [[nodiscard]] float lastPeak() const noexcept;

    /// True if `render()` found the buffer dry since the last check (clears it).
    [[nodiscard]] bool consumeUnderrunFlag() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    TrackMixState mix_;
};

}  // namespace aw::audio
