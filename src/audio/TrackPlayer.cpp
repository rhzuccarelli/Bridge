#include "aw/audio/TrackPlayer.hpp"

#include "aw/core/RingBuffer.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace aw::audio {
namespace {

constexpr core::FrameCount kStreamBlockFrames = 512;
constexpr std::size_t kStreamBlockCount = 16;
constexpr core::ChannelCount kMaxSourceChannels = 12;

struct StreamBlock {
    core::FrameIndex timelineStart{0};
    core::FrameCount frames{0};
    std::uint64_t generation{0};
    core::ChannelCount channels{0};
    std::array<float, static_cast<std::size_t>(kStreamBlockFrames) * kMaxSourceChannels> samples{};
};

static_assert(std::is_trivially_copyable_v<StreamBlock>);

struct SharedStreamSource {
    WavReader reader;
    AudioBuffer streamScratch;
    core::FrameIndex timelineStartFrame{0};
    core::FrameIndex sourceStartFrame{0};
    core::FrameCount sourceLength{0};
    core::FrameIndex producerTimeline{0};
    std::uint64_t producerGeneration{0};
    core::RingBuffer<StreamBlock, kStreamBlockCount> blocks;
    // The two most recent blocks, so a render that straddles a block boundary
    // still finds its frames. A block is 24 KiB: these rotate an index and the
    // ring is drained with popInto(), because copying blocks through the stack
    // overflows the 16 KiB thread stacks of the ESP32-P4.
    std::array<StreamBlock, 2> held{};
    std::array<bool, 2> heldValid{};
    std::size_t currentHeld{0};  ///< Index of the newer block; the other is older.
    std::atomic<core::FrameIndex> requestedTimeline{0};
    std::atomic<std::uint64_t> requestedGeneration{0};
    core::FrameRange loopRange{};
    bool loopEnabled{false};
    std::atomic<bool> underrun{false};
};

bool validRoute(core::ChannelCount first,
                core::ChannelCount count,
                core::ChannelCount available) noexcept {
    return (count == 1 || count == 2) && first < available && count <= available - first;
}

bool validDestination(core::ChannelCount first, core::ChannelCount count) noexcept {
    return (count == 1 || count == 2) && first < 2 && count <= 2 - first;
}

}  // namespace

struct TrackPlayer::Impl {
    std::shared_ptr<SharedStreamSource> source;
    core::ChannelCount sourceChannel{0};
    core::ChannelCount sourceChannelCount{2};
    core::ChannelCount destinationChannel{0};
    core::ChannelCount destinationChannelCount{2};
    std::atomic<float> lastPeak{0.0F};
};

TrackPlayer::TrackPlayer() : impl_{std::make_unique<Impl>()} {}
TrackPlayer::~TrackPlayer() = default;

core::Status TrackPlayer::setSource(hal::StorageInterface& storage,
                                    const core::Path& path,
                                    core::ChannelCount sourceChannel,
                                    core::ChannelCount sourceChannelCount,
                                    core::ChannelCount destinationChannel,
                                    core::ChannelCount destinationChannelCount) {
    auto source = std::make_shared<SharedStreamSource>();
    if (auto status = source->reader.open(storage, path); !status) return status;
    const core::ChannelCount available = source->reader.metadata().format.channels;
    if (!validRoute(sourceChannel, sourceChannelCount, available)) {
        return core::makeError(core::ErrorCode::InvalidArgument,
                               "source channel selection is outside WAV channel range");
    }
    if (!validDestination(destinationChannel, destinationChannelCount)) {
        return core::makeError(core::ErrorCode::InvalidArgument, "invalid destination channel");
    }
    source->streamScratch = AudioBuffer{available, kStreamBlockFrames};
    impl_->source = std::move(source);
    impl_->sourceChannel = sourceChannel;
    impl_->sourceChannelCount = sourceChannelCount;
    impl_->destinationChannel = destinationChannel;
    impl_->destinationChannelCount = destinationChannelCount;
    impl_->lastPeak.store(0.0F, std::memory_order_relaxed);
    return core::success();
}

core::Status TrackPlayer::shareSource(const TrackPlayer& sourceOwner,
                                      core::ChannelCount sourceChannel,
                                      core::ChannelCount sourceChannelCount,
                                      core::ChannelCount destinationChannel,
                                      core::ChannelCount destinationChannelCount) {
    if (!sourceOwner.impl_->source || !sourceOwner.impl_->source->reader.isOpen()) {
        return core::makeError(core::ErrorCode::InvalidState, "source owner has no stream");
    }
    const core::ChannelCount available =
        sourceOwner.impl_->source->reader.metadata().format.channels;
    if (!validRoute(sourceChannel, sourceChannelCount, available)) {
        return core::makeError(core::ErrorCode::InvalidArgument,
                               "source channel selection is outside WAV channel range");
    }
    if (!validDestination(destinationChannel, destinationChannelCount)) {
        return core::makeError(core::ErrorCode::InvalidArgument, "invalid destination channel");
    }
    impl_->source = sourceOwner.impl_->source;
    impl_->sourceChannel = sourceChannel;
    impl_->sourceChannelCount = sourceChannelCount;
    impl_->destinationChannel = destinationChannel;
    impl_->destinationChannelCount = destinationChannelCount;
    impl_->lastPeak.store(0.0F, std::memory_order_relaxed);
    return core::success();
}

void TrackPlayer::clearSource() noexcept { impl_->source.reset(); }

bool TrackPlayer::hasSource() const noexcept {
    return impl_->source && impl_->source->reader.isOpen();
}

core::Status TrackPlayer::prefetch(core::FrameIndex timelinePosition, core::FrameRange sourceRange) {
    if (!hasSource()) {
        return core::makeError(core::ErrorCode::InvalidState, "track has no source");
    }
    SharedStreamSource& source = *impl_->source;
    source.timelineStartFrame = timelinePosition;
    const WavMetadata& metadata = source.reader.metadata();
    const core::FrameIndex sourceStart =
        std::clamp(sourceRange.start, core::FrameIndex{0}, metadata.frameCount);
    const core::FrameCount availableFrames = metadata.frameCount - sourceStart;
    source.sourceLength = sourceRange.length > 0
                              ? std::min(sourceRange.length, availableFrames)
                              : availableFrames;
    source.sourceStartFrame = sourceStart;
    source.producerTimeline = timelinePosition;
    source.requestedTimeline.store(timelinePosition, std::memory_order_relaxed);
    source.requestedGeneration.store(0, std::memory_order_relaxed);
    source.producerGeneration = 0;
    source.heldValid = {false, false};
    if (auto status = source.reader.seek(source.sourceStartFrame); !status) return status;
    (void)stream();
    return core::success();
}

bool TrackPlayer::stream() {
    if (!hasSource()) return false;
    SharedStreamSource& source = *impl_->source;
    const std::uint64_t requestedGeneration =
        source.requestedGeneration.load(std::memory_order_acquire);
    if (requestedGeneration != source.producerGeneration) {
        const core::FrameIndex requested =
            source.requestedTimeline.load(std::memory_order_acquire);
        const core::FrameIndex relative = std::clamp<core::FrameIndex>(
            requested - source.timelineStartFrame, 0, source.sourceLength);
        if (auto status = source.reader.seek(source.sourceStartFrame + relative); !status) {
            source.underrun.store(true, std::memory_order_release);
            return false;
        }
        source.producerTimeline = source.timelineStartFrame + relative;
        source.producerGeneration = requestedGeneration;
    }

    bool produced = false;
    const core::FrameIndex timelineEnd = source.timelineStartFrame + source.sourceLength;
    while (source.blocks.size() < source.blocks.capacity()) {
        if (source.producerTimeline >= timelineEnd) {
            if (source.loopEnabled && source.loopRange.length > 0 &&
                source.producerTimeline < source.loopRange.end()) {
                StreamBlock* silence = source.blocks.reserve();
                if (silence == nullptr) break;
                silence->timelineStart = source.producerTimeline;
                silence->frames = std::min(kStreamBlockFrames,
                                           source.loopRange.end() - source.producerTimeline);
                silence->generation = source.producerGeneration;
                silence->channels = source.reader.metadata().format.channels;
                silence->samples.fill(0.0F);
                const core::FrameCount silenceFrames = silence->frames;
                source.blocks.commit();
                source.producerTimeline += silenceFrames;
                produced = true;
                continue;
            }
            const core::FrameIndex loopedStart =
                std::max(source.loopRange.start, source.timelineStartFrame);
            if (!source.loopEnabled || source.loopRange.length <= 0 ||
                loopedStart >= source.loopRange.end() || loopedStart >= timelineEnd) break;
            source.producerTimeline = loopedStart;
            const core::FrameIndex relative = loopedStart - source.timelineStartFrame;
            if (auto status = source.reader.seek(source.sourceStartFrame + relative); !status) {
                source.underrun.store(true, std::memory_order_release);
                break;
            }
        }
        const core::FrameCount wanted = std::min(
            {kStreamBlockFrames, timelineEnd - source.producerTimeline,
             source.loopEnabled && source.loopRange.length > 0
                 ? std::max<core::FrameCount>(0, source.loopRange.end() - source.producerTimeline)
                 : kStreamBlockFrames});
        if (wanted <= 0) {
            source.producerTimeline = timelineEnd;
            continue;
        }
        StreamBlock* block = source.blocks.reserve();
        if (block == nullptr) break;
        source.streamScratch.setFrameCount(wanted);
        auto read = source.reader.read(source.streamScratch);
        if (!read || read.value() == 0) {
            if (!read) source.underrun.store(true, std::memory_order_release);
            break;
        }
        block->timelineStart = source.producerTimeline;
        block->frames = read.value();
        block->generation = source.producerGeneration;
        block->channels = source.streamScratch.channelCount();
        for (core::ChannelCount channel = 0; channel < block->channels; ++channel) {
            const auto samples = source.streamScratch.channel(channel);
            std::copy(samples.begin(), samples.end(),
                      block->samples.begin() + static_cast<std::ptrdiff_t>(channel) *
                                                   kStreamBlockFrames);
        }
        source.blocks.commit();
        source.producerTimeline += read.value();
        produced = true;
    }
    return produced;
}

void TrackPlayer::setLoop(core::FrameRange range, bool enabled) noexcept {
    if (impl_->source) {
        impl_->source->loopRange = range;
        impl_->source->loopEnabled = enabled;
    }
}

void TrackPlayer::requestStreamPosition(core::FrameIndex timelinePosition) noexcept {
    if (!impl_->source) return;
    SharedStreamSource& source = *impl_->source;
    source.requestedTimeline.store(timelinePosition, std::memory_order_relaxed);
    source.requestedGeneration.fetch_add(1, std::memory_order_release);
    source.heldValid = {false, false};
    // This is the ring's consumer side. Remove prefetched blocks from the old
    // generation immediately so the producer has capacity to refill the new
    // position before Play resumes.
    while (source.blocks.discard()) {}
}

void TrackPlayer::render(AudioBuffer& destination,
                         core::FrameIndex timelinePosition,
                         core::FrameCount frames) noexcept {
    destination.setFrameCount(frames);
    if (!impl_->source || impl_->source->sourceLength == 0 ||
        destination.channelCount() == 0) {
        impl_->lastPeak.store(0.0F, std::memory_order_relaxed);
        return;
    }
    SharedStreamSource& source = *impl_->source;
    const std::uint64_t generation =
        source.requestedGeneration.load(std::memory_order_acquire);
    const float gain = mix_.gain.load(std::memory_order_relaxed);
    const float pan = std::clamp(mix_.pan.load(std::memory_order_relaxed), -1.0F, 1.0F);
    const float leftPan = pan <= 0.0F ? 1.0F : 1.0F - pan;
    const float rightPan = pan >= 0.0F ? 1.0F : 1.0F + pan;
    auto outputLeft = destination.channel(0);
    auto outputRight = destination.channel(destination.channelCount() > 1 ? 1 : 0);

    float peak = 0.0F;
    const auto contains = [&source, generation](std::size_t index, core::FrameIndex position) {
        const StreamBlock& block = source.held[index];
        return source.heldValid[index] && block.generation == generation &&
               position >= block.timelineStart && position < block.timelineStart + block.frames;
    };
    for (core::FrameCount frame = 0; frame < frames; ++frame) {
        const core::FrameIndex desired = timelinePosition + frame;
        std::size_t current = source.currentHeld;
        while (!contains(1 - current, desired) && !contains(current, desired)) {
            // The newest block goes into the older slot, which then becomes current.
            const std::size_t older = 1 - current;
            if (!source.blocks.popInto(source.held[older])) break;
            source.heldValid[older] = true;
            source.currentHeld = current = older;
            if (source.held[current].timelineStart > desired) break;
        }
        const StreamBlock* block = contains(current, desired)
                                       ? &source.held[current]
                                       : (contains(1 - current, desired)
                                              ? &source.held[1 - current]
                                              : nullptr);
        if (block == nullptr) {
            if (desired >= source.timelineStartFrame &&
                desired < source.timelineStartFrame + source.sourceLength) {
                source.underrun.store(true, std::memory_order_relaxed);
            }
            continue;
        }
        const std::size_t index =
            static_cast<std::size_t>(desired - block->timelineStart);
        const std::size_t leftOffset =
            static_cast<std::size_t>(impl_->sourceChannel) * kStreamBlockFrames;
        const std::size_t rightOffset = impl_->sourceChannelCount > 1
                                            ? leftOffset + kStreamBlockFrames
                                            : leftOffset;
        const float mixedLeft = block->samples[leftOffset + index] * gain * leftPan;
        const float mixedRight = block->samples[rightOffset + index] * gain * rightPan;
        if (impl_->destinationChannelCount == 2) {
            outputLeft[static_cast<std::size_t>(frame)] += mixedLeft;
            outputRight[static_cast<std::size_t>(frame)] += mixedRight;
        } else if (impl_->destinationChannel == 0) {
            outputLeft[static_cast<std::size_t>(frame)] += mixedLeft;
        } else {
            outputRight[static_cast<std::size_t>(frame)] += mixedLeft;
        }
        peak = std::max(peak, std::max(std::fabs(mixedLeft), std::fabs(mixedRight)));
    }
    impl_->lastPeak.store(peak, std::memory_order_relaxed);
}

float TrackPlayer::lastPeak() const noexcept {
    return impl_->lastPeak.load(std::memory_order_relaxed);
}

bool TrackPlayer::consumeUnderrunFlag() noexcept {
    return impl_->source &&
           impl_->source->underrun.exchange(false, std::memory_order_relaxed);
}

}  // namespace aw::audio
