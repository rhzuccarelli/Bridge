#include "aw/audio/Transport.hpp"

// The control side is implemented now because it is pure state and the UI needs
// something real to drive in Phase 1.  `advance()` (the realtime half) lands in
// Phase 4 alongside the mixer.

namespace aw::audio {

void Transport::requestPlay() noexcept {
    const State next = isRecordArmed() ? State::Recording : State::Playing;
    state_.store(next, std::memory_order_release);
}

void Transport::requestStop() noexcept {
    state_.store(State::Stopped, std::memory_order_release);
    pendingSeek_.store(0, std::memory_order_release);
}

void Transport::requestPause() noexcept { state_.store(State::Paused, std::memory_order_release); }

void Transport::requestRecord(core::TrackId track) noexcept {
    armedTrack_.store(track, std::memory_order_release);
}

void Transport::disarmRecord() noexcept {
    armedTrack_.store(core::kInvalidTrackId, std::memory_order_release);
    if (state() == State::Recording) {
        state_.store(State::Playing, std::memory_order_release);
    }
}

void Transport::requestSeek(core::FrameIndex frame) noexcept {
    pendingSeek_.store(frame < 0 ? 0 : frame, std::memory_order_release);
    seekRevision_.fetch_add(1, std::memory_order_release);
}

void Transport::setLoop(core::FrameRange range, bool enabled) noexcept {
    loopStart_.store(range.start, std::memory_order_relaxed);
    loopLength_.store(range.length, std::memory_order_relaxed);
    loopEnabled_.store(enabled, std::memory_order_release);
}

Transport::State Transport::state() const noexcept {
    return state_.load(std::memory_order_acquire);
}

core::FrameIndex Transport::position() const noexcept {
    return position_.load(std::memory_order_acquire);
}

bool Transport::isLoopEnabled() const noexcept {
    return loopEnabled_.load(std::memory_order_acquire);
}

core::FrameRange Transport::loopRange() const noexcept {
    return core::FrameRange{loopStart_.load(std::memory_order_relaxed),
                            loopLength_.load(std::memory_order_relaxed)};
}

bool Transport::isRecordArmed() const noexcept {
    return armedTrack_.load(std::memory_order_acquire) != core::kInvalidTrackId;
}

core::TrackId Transport::armedTrack() const noexcept {
    return armedTrack_.load(std::memory_order_acquire);
}

std::uint64_t Transport::seekRevision() const noexcept {
    return seekRevision_.load(std::memory_order_acquire);
}

core::FrameIndex Transport::advance(core::FrameCount frames) noexcept {
    if (const core::FrameIndex seek = pendingSeek_.exchange(-1, std::memory_order_acq_rel);
        seek >= 0) {
        position_.store(seek, std::memory_order_release);
    }

    const core::FrameIndex blockStart = position_.load(std::memory_order_acquire);
    const State currentState = state_.load(std::memory_order_acquire);
    if (currentState != State::Playing && currentState != State::Recording) {
        return blockStart;
    }

    core::FrameIndex next = blockStart + frames;
    if (loopEnabled_.load(std::memory_order_acquire)) {
        const core::FrameIndex start = loopStart_.load(std::memory_order_relaxed);
        const core::FrameCount length = loopLength_.load(std::memory_order_relaxed);
        if (length > 0) {
            const core::FrameIndex end = start + length;
            while (next >= end) {
                next = start + (next - end);
            }
        }
    }

    position_.store(next, std::memory_order_release);
    return blockStart;
}

}  // namespace aw::audio
