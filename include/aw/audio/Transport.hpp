#pragma once

#include <atomic>

#include "aw/core/Types.hpp"
#include "aw/events/Event.hpp"

namespace aw::audio {

/// The playhead and its state machine.
///
/// Owned by the engine and read from the audio callback, so every mutable field
/// is a lock free atomic.  Control changes (play, stop, seek) are *requests*
/// posted from the UI thread and applied by the audio thread at a block
/// boundary; that is why there is no "set position now" method that a caller
/// could use to move the playhead out from under a running render.
class Transport {
public:
    using State = events::TransportState;

    Transport() = default;

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    // --- Control side (UI thread) -------------------------------------------
    void requestPlay() noexcept;
    void requestStop() noexcept;
    void requestPause() noexcept;
    /// Portastudio semantics: arms a track but does not roll. A following Play
    /// enters Recording; Stop leaves the track armed for another take.
    void requestRecord(core::TrackId track) noexcept;
    void disarmRecord() noexcept;
    void requestSeek(core::FrameIndex frame) noexcept;
    void setLoop(core::FrameRange range, bool enabled) noexcept;

    // --- Observation (any thread) -------------------------------------------
    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] core::FrameIndex position() const noexcept;
    [[nodiscard]] bool isLoopEnabled() const noexcept;
    [[nodiscard]] core::FrameRange loopRange() const noexcept;
    [[nodiscard]] bool isRecordArmed() const noexcept;
    [[nodiscard]] core::TrackId armedTrack() const noexcept;
    [[nodiscard]] std::uint64_t seekRevision() const noexcept;

    // --- Realtime side (audio thread only) ----------------------------------
    /// Applies pending requests and advances the playhead by `frames`,
    /// wrapping at the loop point.  Returns the position the block starts at.
    core::FrameIndex advance(core::FrameCount frames) noexcept;

private:
    std::atomic<State> state_{State::Stopped};
    std::atomic<core::FrameIndex> position_{0};
    std::atomic<core::FrameIndex> pendingSeek_{-1};  ///< -1 = none pending.
    std::atomic<bool> loopEnabled_{false};
    std::atomic<core::FrameIndex> loopStart_{0};
    std::atomic<core::FrameCount> loopLength_{0};
    std::atomic<core::TrackId> armedTrack_{core::kInvalidTrackId};
    std::atomic<std::uint64_t> seekRevision_{0};
};

}  // namespace aw::audio
