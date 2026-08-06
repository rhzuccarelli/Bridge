#include "aw/audio/Transport.hpp"

// The control side is implemented now because it is pure state and the UI needs
// something real to drive in Phase 1.  `advance()` (the realtime half) lands in
// Phase 4 alongside the mixer.

namespace aw::audio {

void Transport::requestPlay() noexcept { state_.store(State::Playing, std::memory_order_release); }

void Transport::requestStop() noexcept {
    state_.store(State::Stopped, std::memory_order_release);
    pendingSeek_.store(0, std::memory_order_release);
}

void Transport::requestPause() noexcept { state_.store(State::Paused, std::memory_order_release); }

void Transport::requestRecord() noexcept {
    state_.store(State::Recording, std::memory_order_release);
}

void Transport::requestSeek(core::FrameIndex frame) noexcept {
    pendingSeek_.store(frame < 0 ? 0 : frame, std::memory_order_release);
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

core::FrameIndex Transport::advance(core::FrameCount /*frames*/) noexcept {
    // Phase 4: apply pendingSeek_, advance position_, wrap at the loop point.
    return position_.load(std::memory_order_relaxed);
}

}  // namespace aw::audio
