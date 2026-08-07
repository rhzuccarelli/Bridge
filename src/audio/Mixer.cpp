#include "aw/audio/Mixer.hpp"

#include <algorithm>
#include <cmath>

namespace aw::audio {

void Mixer::prepare(core::ChannelCount outputChannels, core::FrameCount maxBlockFrames) {
    channels_ = outputChannels;
    maxBlockFrames_ = maxBlockFrames;
    scratch_ = AudioBuffer{outputChannels, maxBlockFrames};
    peaks_.assign(static_cast<std::size_t>(outputChannels), 0.0F);
}

void Mixer::render(std::span<TrackPlayer* const> tracks,
                   AudioBuffer& destination,
                   core::FrameIndex timelinePosition,
                   core::FrameCount frames) noexcept {
    destination.setFrameCount(frames);
    destination.clear();
    std::fill(peaks_.begin(), peaks_.end(), 0.0F);

    const bool anySolo = std::any_of(tracks.begin(), tracks.end(), [](TrackPlayer* track) {
        return track != nullptr && track->mix().soloed.load(std::memory_order_relaxed);
    });

    for (TrackPlayer* track : tracks) {
        if (track == nullptr) {
            continue;
        }

        const bool soloed = track->mix().soloed.load(std::memory_order_relaxed);
        const bool muted = track->mix().muted.load(std::memory_order_relaxed);
        if ((anySolo && !soloed) || muted) {
            continue;
        }
        track->render(destination, timelinePosition, frames);
    }

    for (core::ChannelCount channel = 0; channel < channels_; ++channel) {
        auto samples = destination.channel(channel);
        float peak = 0.0F;
        for (float& sample : samples) {
            sample *= masterGain_;
            peak = std::max(peak, std::fabs(sample));
        }
        peaks_[channel] = peak;
    }
}

void Mixer::setMasterGain(float gain) noexcept { masterGain_ = std::clamp(gain, 0.0F, 4.0F); }

float Mixer::masterGain() const noexcept { return masterGain_; }

float Mixer::lastPeak(core::ChannelCount channel) const noexcept {
    return channel < peaks_.size() ? peaks_[channel] : 0.0F;
}

}  // namespace aw::audio
