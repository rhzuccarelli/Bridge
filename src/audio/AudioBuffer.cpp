#include "aw/audio/AudioBuffer.hpp"

#include <algorithm>
#include <cassert>

namespace aw::audio {

AudioBuffer::AudioBuffer(core::ChannelCount channels, core::FrameCount capacityFrames)
    : channels_{channels},
      capacity_{capacityFrames},
      frames_{capacityFrames},
      samples_(static_cast<std::size_t>(channels) * static_cast<std::size_t>(capacityFrames),
               0.0F) {}

void AudioBuffer::setFrameCount(core::FrameCount frames) noexcept {
    assert(frames >= 0 && frames <= capacity_);
    frames_ = std::clamp<core::FrameCount>(frames, 0, capacity_);
}

std::span<float> AudioBuffer::channel(core::ChannelCount channel) noexcept {
    assert(channel < channels_);
    const std::size_t offset = static_cast<std::size_t>(channel) * static_cast<std::size_t>(capacity_);
    return std::span<float>{samples_.data() + offset, static_cast<std::size_t>(frames_)};
}

std::span<const float> AudioBuffer::channel(core::ChannelCount channel) const noexcept {
    assert(channel < channels_);
    const std::size_t offset = static_cast<std::size_t>(channel) * static_cast<std::size_t>(capacity_);
    return std::span<const float>{samples_.data() + offset, static_cast<std::size_t>(frames_)};
}

void AudioBuffer::clear() noexcept {
    for (core::ChannelCount c = 0; c < channels_; ++c) {
        std::span<float> data = channel(c);
        std::fill(data.begin(), data.end(), 0.0F);
    }
}

}  // namespace aw::audio
