#include "aw/audio/AudioEngine.hpp"

#include <algorithm>

namespace aw::audio {
namespace {

float clampSample(float value) noexcept { return std::clamp(value, -1.0F, 1.0F); }

}  // namespace

/// Engine internals.  Phase 4 adds the streaming thread and the double buffered
/// scene swap here.
struct AudioEngine::Impl {
    std::vector<std::unique_ptr<TrackPlayer>> players;
    std::vector<TrackPlayer*> activePlayers;  ///< Read by the audio thread.
    std::vector<core::TrackId> trackIds;
    AudioBuffer mixBuffer;
    bool running{false};
    events::TransportState lastTransportState{events::TransportState::Stopped};
    bool hasPostedTransportState{false};
};

AudioEngine::AudioEngine(hal::AudioOutputInterface& output,
                         hal::StorageInterface& storage,
                         events::EventQueue& eventQueue)
    : output_{output},
      storage_{storage},
      eventQueue_{eventQueue},
      impl_{std::make_unique<Impl>()} {}

AudioEngine::~AudioEngine() { stop(); }

core::Status AudioEngine::start(core::FrameCount bufferFrames) {
    if (impl_->running) {
        return core::makeError(core::ErrorCode::InvalidState, "AudioEngine already started");
    }

    mixer_.prepare(core::kNativeFormat.channels, bufferFrames);
    impl_->mixBuffer = AudioBuffer{core::kNativeFormat.channels, bufferFrames};
    impl_->hasPostedTransportState = false;

    if (auto status = output_.open(core::kNativeFormat, bufferFrames, *this); !status) {
        return status;
    }
    if (auto status = output_.start(); !status) {
        output_.close();
        return status;
    }

    impl_->running = true;
    return core::success();
}

void AudioEngine::stop() noexcept {
    if (!impl_->running) {
        return;
    }
    output_.stop();
    output_.close();
    impl_->running = false;
}

bool AudioEngine::isRunning() const noexcept { return impl_->running; }

core::Status AudioEngine::setScene(const EngineScene& scene) {
    std::vector<std::unique_ptr<TrackPlayer>> players;
    std::vector<TrackPlayer*> active;
    std::vector<core::TrackId> ids;
    players.reserve(scene.tracks.size());
    active.reserve(scene.tracks.size());
    ids.reserve(scene.tracks.size());

    for (const EngineTrack& track : scene.tracks) {
        auto player = std::make_unique<TrackPlayer>();
        if (auto status = player->setSource(storage_, track.audioFile); !status) {
            return status;
        }
        player->mix().gain.store(track.gain, std::memory_order_relaxed);
        player->mix().pan.store(track.pan, std::memory_order_relaxed);
        player->mix().muted.store(track.muted, std::memory_order_relaxed);
        player->mix().soloed.store(track.soloed, std::memory_order_relaxed);
        if (auto status = player->prefetch(track.startFrame, track.sourceRange); !status) {
            return status;
        }

        ids.push_back(track.id);
        active.push_back(player.get());
        players.push_back(std::move(player));
    }

    transport_.setLoop(scene.loopRange, scene.loopEnabled);
    impl_->trackIds = std::move(ids);
    impl_->activePlayers = std::move(active);
    impl_->players = std::move(players);
    return core::success();
}

void AudioEngine::setTrackGain(core::TrackId id, float gain) noexcept {
    for (std::size_t i = 0; i < impl_->trackIds.size(); ++i) {
        if (impl_->trackIds[i] == id) {
            impl_->players[i]->mix().gain.store(gain, std::memory_order_relaxed);
        }
    }
}
void AudioEngine::setTrackPan(core::TrackId id, float pan) noexcept {
    for (std::size_t i = 0; i < impl_->trackIds.size(); ++i) {
        if (impl_->trackIds[i] == id) {
            impl_->players[i]->mix().pan.store(pan, std::memory_order_relaxed);
        }
    }
}
void AudioEngine::setTrackMuted(core::TrackId id, bool muted) noexcept {
    for (std::size_t i = 0; i < impl_->trackIds.size(); ++i) {
        if (impl_->trackIds[i] == id) {
            impl_->players[i]->mix().muted.store(muted, std::memory_order_relaxed);
        }
    }
}
void AudioEngine::setTrackSoloed(core::TrackId id, bool soloed) noexcept {
    for (std::size_t i = 0; i < impl_->trackIds.size(); ++i) {
        if (impl_->trackIds[i] == id) {
            impl_->players[i]->mix().soloed.store(soloed, std::memory_order_relaxed);
        }
    }
}

void AudioEngine::setMasterGain(float gain) noexcept { mixer_.setMasterGain(gain); }

void AudioEngine::renderAudio(std::span<float> output,
                              core::FrameCount frames,
                              core::ChannelCount channels) noexcept {
    const auto sampleCount =
        static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels);
    const std::size_t limit = std::min(sampleCount, output.size());
    std::fill_n(output.begin(), limit, 0.0F);

    const core::FrameIndex blockStart = transport_.advance(frames);
    mixer_.render(impl_->activePlayers, impl_->mixBuffer, blockStart, frames);

    const core::ChannelCount renderChannels =
        std::min<core::ChannelCount>(channels, impl_->mixBuffer.channelCount());
    for (core::FrameCount frame = 0; frame < frames; ++frame) {
        for (core::ChannelCount channel = 0; channel < renderChannels; ++channel) {
            const std::size_t index = static_cast<std::size_t>(frame) * channels + channel;
            if (index < output.size()) {
                output[index] =
                    clampSample(impl_->mixBuffer.channel(channel)[static_cast<std::size_t>(frame)]);
            }
        }
    }

    const auto transportState = transport_.state();
    const auto position = transport_.position();
    if (!impl_->hasPostedTransportState || transportState != impl_->lastTransportState) {
        eventQueue_.post(events::TransportEvent{transportState, position});
        impl_->lastTransportState = transportState;
        impl_->hasPostedTransportState = true;
    }

    for (std::size_t i = 0; i < impl_->trackIds.size(); ++i) {
        bool seen = false;
        for (std::size_t prior = 0; prior < i; ++prior) {
            if (impl_->trackIds[prior] == impl_->trackIds[i]) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }

        float peak = 0.0F;
        for (std::size_t candidate = i; candidate < impl_->trackIds.size(); ++candidate) {
            if (impl_->trackIds[candidate] == impl_->trackIds[i]) {
                peak = std::max(peak, impl_->players[candidate]->lastPeak());
            }
        }
        eventQueue_.post(events::LevelEvent{impl_->trackIds[i], peak});
    }

    eventQueue_.post(events::PositionEvent{position});
}

void AudioEngine::onXRun() noexcept {
    eventQueue_.post(events::NotificationEvent{core::ErrorCode::Timeout, true});
}

}  // namespace aw::audio
