#include "aw/audio/TrackPlayer.hpp"

#include <algorithm>
#include <cmath>

namespace aw::audio {

/// Streaming state.  Phase 4 adds the disk->audio ring buffer here; keeping it
/// in the pimpl means the ring's size and layout can change without recompiling
/// every user of TrackPlayer.
struct TrackPlayer::Impl {
    WavReader reader;
    core::FrameIndex sourceStartFrame{0};
    AudioBuffer source;
    std::atomic<float> lastPeak{0.0F};
    std::atomic<bool> underrun{false};
};

TrackPlayer::TrackPlayer() : impl_{std::make_unique<Impl>()} {}
TrackPlayer::~TrackPlayer() = default;

core::Status TrackPlayer::setSource(hal::StorageInterface& storage, const core::Path& path) {
    impl_->source = AudioBuffer{};
    impl_->sourceStartFrame = 0;
    impl_->lastPeak.store(0.0F, std::memory_order_relaxed);
    impl_->underrun.store(false, std::memory_order_relaxed);
    return impl_->reader.open(storage, path);
}

void TrackPlayer::clearSource() noexcept { impl_->reader.close(); }

bool TrackPlayer::hasSource() const noexcept { return impl_->reader.isOpen(); }

core::Status TrackPlayer::prefetch(core::FrameIndex timelinePosition, core::FrameRange sourceRange) {
    if (!hasSource()) {
        return core::makeError(core::ErrorCode::InvalidState, "track has no source");
    }

    impl_->sourceStartFrame = timelinePosition;
    const WavMetadata& metadata = impl_->reader.metadata();
    const core::FrameIndex sourceStart = std::clamp(sourceRange.start, core::FrameIndex{0},
                                                    metadata.frameCount);
    const core::FrameCount availableFrames = metadata.frameCount - sourceStart;
    const core::FrameCount framesToLoad =
        sourceRange.length > 0 ? std::min(sourceRange.length, availableFrames) : availableFrames;
    impl_->source = AudioBuffer{metadata.format.channels, framesToLoad};
    impl_->source.setFrameCount(framesToLoad);

    if (auto status = impl_->reader.seek(sourceStart); !status) {
        return status;
    }

    core::FrameCount loaded = 0;
    while (loaded < framesToLoad) {
        AudioBuffer chunk{metadata.format.channels, framesToLoad - loaded};
        auto read = impl_->reader.read(chunk);
        if (!read) {
            return read.error();
        }
        if (read.value() == 0) {
            break;
        }

        for (core::ChannelCount channel = 0; channel < metadata.format.channels; ++channel) {
            auto destination = impl_->source.channel(channel);
            auto source = chunk.channel(channel);
            std::copy(source.begin(), source.end(),
                      destination.begin() + static_cast<std::ptrdiff_t>(loaded));
        }
        loaded += read.value();
    }

    impl_->source.setFrameCount(loaded);
    return core::success();
}

void TrackPlayer::render(AudioBuffer& destination,
                         core::FrameIndex timelinePosition,
                         core::FrameCount frames) noexcept {
    destination.setFrameCount(frames);
    if (impl_->source.frameCount() == 0 || destination.channelCount() == 0) {
        impl_->lastPeak.store(0.0F, std::memory_order_relaxed);
        return;
    }

    const core::FrameIndex relativeStart = timelinePosition - impl_->sourceStartFrame;
    const float gain = mix_.gain.load(std::memory_order_relaxed);
    const float pan = std::clamp(mix_.pan.load(std::memory_order_relaxed), -1.0F, 1.0F);
    const bool muted = mix_.muted.load(std::memory_order_relaxed);
    const float leftPan = pan <= 0.0F ? 1.0F : 1.0F - pan;
    const float rightPan = pan >= 0.0F ? 1.0F : 1.0F + pan;

    auto outputLeft = destination.channel(0);
    auto outputRight = destination.channel(destination.channelCount() > 1 ? 1 : 0);

    float peak = 0.0F;
    for (core::FrameCount frame = 0; frame < frames; ++frame) {
        const core::FrameIndex sourceFrame = relativeStart + frame;
        if (sourceFrame < 0 || sourceFrame >= impl_->source.frameCount()) {
            continue;
        }

        const std::size_t index = static_cast<std::size_t>(sourceFrame);
        const float left = impl_->source.channel(0)[index];
        const float right =
            impl_->source.channel(impl_->source.channelCount() > 1 ? 1 : 0)[index];

        const float mixedLeft = muted ? 0.0F : left * gain * leftPan;
        const float mixedRight = muted ? 0.0F : right * gain * rightPan;

        outputLeft[static_cast<std::size_t>(frame)] += mixedLeft;
        outputRight[static_cast<std::size_t>(frame)] += mixedRight;
        peak = std::max(peak, std::max(std::fabs(mixedLeft), std::fabs(mixedRight)));
    }

    if (relativeStart + frames > impl_->source.frameCount()) {
        impl_->underrun.store(true, std::memory_order_relaxed);
    }
    impl_->lastPeak.store(peak, std::memory_order_relaxed);
}

float TrackPlayer::lastPeak() const noexcept {
    return impl_->lastPeak.load(std::memory_order_relaxed);
}

bool TrackPlayer::consumeUnderrunFlag() noexcept {
    return impl_->underrun.exchange(false, std::memory_order_relaxed);
}

}  // namespace aw::audio
