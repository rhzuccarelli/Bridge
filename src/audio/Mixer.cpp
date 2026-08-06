#include "aw/audio/Mixer.hpp"

#include <algorithm>

namespace aw::audio {

void Mixer::prepare(core::ChannelCount outputChannels, core::FrameCount maxBlockFrames) {
    channels_ = outputChannels;
    maxBlockFrames_ = maxBlockFrames;
    scratch_ = AudioBuffer{outputChannels, maxBlockFrames};
    peaks_.assign(static_cast<std::size_t>(outputChannels), 0.0F);
}

void Mixer::render(std::span<TrackPlayer* const> /*tracks*/,
                   AudioBuffer& destination,
                   core::FrameIndex /*timelinePosition*/,
                   core::FrameCount frames) noexcept {
    // Phase 4 sums the tracks here.  Clearing the destination is unconditional
    // and stays that way: the output must be silent, not stale, whenever there
    // is nothing to play.
    destination.setFrameCount(frames);
    destination.clear();
    std::fill(peaks_.begin(), peaks_.end(), 0.0F);
}

void Mixer::setMasterGain(float gain) noexcept { masterGain_ = std::clamp(gain, 0.0F, 4.0F); }

float Mixer::masterGain() const noexcept { return masterGain_; }

float Mixer::lastPeak(core::ChannelCount channel) const noexcept {
    return channel < peaks_.size() ? peaks_[channel] : 0.0F;
}

}  // namespace aw::audio
