#include "aw/audio/TrackPlayer.hpp"

#include "aw/core/RingBuffer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace aw::audio {

namespace {

constexpr core::FrameCount kStreamBlockFrames = 512;
constexpr std::size_t kStreamBlockCount = 16;

struct StreamBlock {
    core::FrameIndex timelineStart{0};
    core::FrameCount frames{0};
    std::uint64_t generation{0};
    core::ChannelCount channels{0};
    std::array<float, static_cast<std::size_t>(kStreamBlockFrames) * 2U> samples{};
};

static_assert(std::is_trivially_copyable_v<StreamBlock>);

}  // namespace

struct TrackPlayer::Impl {
    WavReader reader;
    AudioBuffer streamScratch;
    core::FrameIndex timelineStartFrame{0};
    core::FrameIndex sourceStartFrame{0};
    core::FrameCount sourceLength{0};
    core::FrameIndex producerTimeline{0};
    std::uint64_t producerGeneration{0};
    core::RingBuffer<StreamBlock, kStreamBlockCount> blocks;
    std::optional<StreamBlock> currentBlock;
    std::atomic<core::FrameIndex> requestedTimeline{0};
    std::atomic<std::uint64_t> requestedGeneration{0};
    core::FrameIndex expectedRenderTimeline{0};
    core::FrameRange loopRange{};
    bool loopEnabled{false};
    std::atomic<float> lastPeak{0.0F};
    std::atomic<bool> underrun{false};
};

TrackPlayer::TrackPlayer() : impl_{std::make_unique<Impl>()} {}
TrackPlayer::~TrackPlayer() = default;

core::Status TrackPlayer::setSource(hal::StorageInterface& storage, const core::Path& path) {
    impl_->currentBlock.reset();
    impl_->timelineStartFrame = 0;
    impl_->sourceStartFrame = 0;
    impl_->sourceLength = 0;
    impl_->lastPeak.store(0.0F, std::memory_order_relaxed);
    impl_->underrun.store(false, std::memory_order_relaxed);
    if (auto status = impl_->reader.open(storage, path); !status) {
        return status;
    }
    impl_->streamScratch =
        AudioBuffer{impl_->reader.metadata().format.channels, kStreamBlockFrames};
    return core::success();
}

void TrackPlayer::clearSource() noexcept { impl_->reader.close(); }

bool TrackPlayer::hasSource() const noexcept { return impl_->reader.isOpen(); }

core::Status TrackPlayer::prefetch(core::FrameIndex timelinePosition, core::FrameRange sourceRange) {
    if (!hasSource()) {
        return core::makeError(core::ErrorCode::InvalidState, "track has no source");
    }

    impl_->timelineStartFrame = timelinePosition;
    const WavMetadata& metadata = impl_->reader.metadata();
    const core::FrameIndex sourceStart = std::clamp(sourceRange.start, core::FrameIndex{0},
                                                    metadata.frameCount);
    const core::FrameCount availableFrames = metadata.frameCount - sourceStart;
    const core::FrameCount framesToLoad =
        sourceRange.length > 0 ? std::min(sourceRange.length, availableFrames) : availableFrames;
    impl_->sourceStartFrame = sourceStart;
    impl_->sourceLength = framesToLoad;
    impl_->producerTimeline = timelinePosition;
    impl_->expectedRenderTimeline = timelinePosition;
    impl_->requestedTimeline.store(timelinePosition, std::memory_order_relaxed);
    impl_->requestedGeneration.store(0, std::memory_order_relaxed);
    impl_->producerGeneration = 0;
    impl_->currentBlock.reset();
    if (auto status = impl_->reader.seek(impl_->sourceStartFrame); !status) {
        return status;
    }
    (void)stream();
    return core::success();
}

bool TrackPlayer::stream() {
    if (!hasSource()) {
        return false;
    }

    const std::uint64_t requestedGeneration =
        impl_->requestedGeneration.load(std::memory_order_acquire);
    if (requestedGeneration != impl_->producerGeneration) {
        const core::FrameIndex requested = impl_->requestedTimeline.load(std::memory_order_acquire);
        const core::FrameIndex relative = std::clamp<core::FrameIndex>(
            requested - impl_->timelineStartFrame, 0, impl_->sourceLength);
        if (auto status = impl_->reader.seek(impl_->sourceStartFrame + relative); !status) {
            impl_->underrun.store(true, std::memory_order_release);
            return false;
        }
        impl_->producerTimeline = impl_->timelineStartFrame + relative;
        impl_->producerGeneration = requestedGeneration;
    }

    bool produced = false;
    const core::FrameIndex timelineEnd = impl_->timelineStartFrame + impl_->sourceLength;
    while (impl_->blocks.size() < impl_->blocks.capacity()) {
        if (impl_->producerTimeline >= timelineEnd) {
            if (impl_->loopEnabled && impl_->loopRange.length > 0 &&
                impl_->producerTimeline < impl_->loopRange.end()) {
                StreamBlock silence;
                silence.timelineStart = impl_->producerTimeline;
                silence.frames = std::min(kStreamBlockFrames,
                                           impl_->loopRange.end() - impl_->producerTimeline);
                silence.generation = impl_->producerGeneration;
                silence.channels = impl_->reader.metadata().format.channels;
                if (!impl_->blocks.push(silence)) {
                    break;
                }
                impl_->producerTimeline += silence.frames;
                produced = true;
                continue;
            }
            const core::FrameIndex loopedStart =
                std::max(impl_->loopRange.start, impl_->timelineStartFrame);
            if (!impl_->loopEnabled || impl_->loopRange.length <= 0 ||
                loopedStart >= impl_->loopRange.end() || loopedStart >= timelineEnd) {
                break;
            }
            impl_->producerTimeline = loopedStart;
            const core::FrameIndex relative = loopedStart - impl_->timelineStartFrame;
            if (auto status = impl_->reader.seek(impl_->sourceStartFrame + relative); !status) {
                impl_->underrun.store(true, std::memory_order_release);
                break;
            }
        }
        const core::FrameCount wanted =
            std::min({kStreamBlockFrames, timelineEnd - impl_->producerTimeline,
                      impl_->loopEnabled && impl_->loopRange.length > 0
                          ? std::max<core::FrameCount>(0,
                                impl_->loopRange.end() - impl_->producerTimeline)
                          : kStreamBlockFrames});
        if (wanted <= 0) {
            impl_->producerTimeline = timelineEnd;
            continue;
        }
        impl_->streamScratch.setFrameCount(wanted);
        auto read = impl_->reader.read(impl_->streamScratch);
        if (!read || read.value() == 0) {
            if (!read) {
                impl_->underrun.store(true, std::memory_order_release);
            }
            break;
        }

        StreamBlock block;
        block.timelineStart = impl_->producerTimeline;
        block.frames = read.value();
        block.generation = impl_->producerGeneration;
        block.channels = impl_->streamScratch.channelCount();
        for (core::ChannelCount channel = 0; channel < block.channels; ++channel) {
            const auto samples = impl_->streamScratch.channel(channel);
            std::copy(samples.begin(), samples.end(),
                      block.samples.begin() + static_cast<std::ptrdiff_t>(channel) *
                                                  kStreamBlockFrames);
        }
        if (!impl_->blocks.push(block)) {
            break;
        }
        impl_->producerTimeline += read.value();
        produced = true;
    }
    return produced;
}

void TrackPlayer::setLoop(core::FrameRange range, bool enabled) noexcept {
    impl_->loopRange = range;
    impl_->loopEnabled = enabled;
}

void TrackPlayer::requestStreamPosition(core::FrameIndex timelinePosition) noexcept {
    impl_->requestedTimeline.store(timelinePosition, std::memory_order_relaxed);
    impl_->requestedGeneration.fetch_add(1, std::memory_order_release);
    impl_->currentBlock.reset();
    impl_->expectedRenderTimeline = timelinePosition;
}

void TrackPlayer::render(AudioBuffer& destination,
                         core::FrameIndex timelinePosition,
                         core::FrameCount frames) noexcept {
    destination.setFrameCount(frames);
    if (impl_->sourceLength == 0 || destination.channelCount() == 0) {
        impl_->lastPeak.store(0.0F, std::memory_order_relaxed);
        return;
    }

    if (timelinePosition != impl_->expectedRenderTimeline) {
        // A loop wrap is already represented in queue order by the producer.
        // Explicit seeks call requestStreamPosition() and change generation.
        impl_->currentBlock.reset();
    }
    impl_->expectedRenderTimeline = timelinePosition + frames;
    const std::uint64_t generation =
        impl_->requestedGeneration.load(std::memory_order_acquire);
    const float gain = mix_.gain.load(std::memory_order_relaxed);
    const float pan = std::clamp(mix_.pan.load(std::memory_order_relaxed), -1.0F, 1.0F);
    const bool muted = mix_.muted.load(std::memory_order_relaxed);
    const float leftPan = pan <= 0.0F ? 1.0F : 1.0F - pan;
    const float rightPan = pan >= 0.0F ? 1.0F : 1.0F + pan;

    auto outputLeft = destination.channel(0);
    auto outputRight = destination.channel(destination.channelCount() > 1 ? 1 : 0);

    float peak = 0.0F;
    for (core::FrameCount frame = 0; frame < frames; ++frame) {
        const core::FrameIndex desired = timelinePosition + frame;
        while (!impl_->currentBlock ||
               desired >= impl_->currentBlock->timelineStart + impl_->currentBlock->frames ||
               impl_->currentBlock->generation != generation) {
            impl_->currentBlock = impl_->blocks.pop();
            if (!impl_->currentBlock) {
                break;
            }
        }
        if (!impl_->currentBlock || impl_->currentBlock->generation != generation ||
            desired < impl_->currentBlock->timelineStart) {
            if (desired >= impl_->timelineStartFrame &&
                desired < impl_->timelineStartFrame + impl_->sourceLength) {
                impl_->underrun.store(true, std::memory_order_relaxed);
            }
            continue;
        }

        const std::size_t index = static_cast<std::size_t>(desired - impl_->currentBlock->timelineStart);
        const float left = impl_->currentBlock->samples[index];
        const std::size_t rightOffset = impl_->currentBlock->channels > 1
                                            ? static_cast<std::size_t>(kStreamBlockFrames)
                                            : 0U;
        const float right = impl_->currentBlock->samples[rightOffset + index];

        const float mixedLeft = muted ? 0.0F : left * gain * leftPan;
        const float mixedRight = muted ? 0.0F : right * gain * rightPan;

        outputLeft[static_cast<std::size_t>(frame)] += mixedLeft;
        outputRight[static_cast<std::size_t>(frame)] += mixedRight;
        peak = std::max(peak, std::max(std::fabs(mixedLeft), std::fabs(mixedRight)));
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
