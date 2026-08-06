#include "aw/audio/TrackPlayer.hpp"

// Phase 4 implements streaming and rendering.

namespace aw::audio {

/// Streaming state.  Phase 4 adds the disk->audio ring buffer here; keeping it
/// in the pimpl means the ring's size and layout can change without recompiling
/// every user of TrackPlayer.
struct TrackPlayer::Impl {
    WavReader reader;
    core::FrameIndex sourceStartFrame{0};
    std::atomic<float> lastPeak{0.0F};
    std::atomic<bool> underrun{false};
};

TrackPlayer::TrackPlayer() : impl_{std::make_unique<Impl>()} {}
TrackPlayer::~TrackPlayer() = default;

core::Status TrackPlayer::setSource(hal::StorageInterface& storage, const core::Path& path) {
    return impl_->reader.open(storage, path);
}

void TrackPlayer::clearSource() noexcept { impl_->reader.close(); }

bool TrackPlayer::hasSource() const noexcept { return impl_->reader.isOpen(); }

core::Status TrackPlayer::prefetch(core::FrameIndex /*timelinePosition*/) {
    return core::notImplemented("TrackPlayer::prefetch (Phase 4)");
}

void TrackPlayer::render(AudioBuffer& /*destination*/,
                         core::FrameIndex /*timelinePosition*/,
                         core::FrameCount /*frames*/) noexcept {
    // Phase 4.  Until then a track contributes nothing, which is why the mixer
    // clears its destination rather than relying on tracks to initialise it.
}

float TrackPlayer::lastPeak() const noexcept {
    return impl_->lastPeak.load(std::memory_order_relaxed);
}

bool TrackPlayer::consumeUnderrunFlag() noexcept {
    return impl_->underrun.exchange(false, std::memory_order_relaxed);
}

}  // namespace aw::audio
