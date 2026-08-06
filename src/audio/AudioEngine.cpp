#include "aw/audio/AudioEngine.hpp"

#include <algorithm>

namespace aw::audio {

/// Engine internals.  Phase 4 adds the streaming thread and the double buffered
/// scene swap here.
struct AudioEngine::Impl {
    std::vector<std::unique_ptr<TrackPlayer>> players;
    std::vector<TrackPlayer*> activePlayers;  ///< Read by the audio thread.
    AudioBuffer mixBuffer;
    bool running{false};
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

core::Status AudioEngine::setScene(const EngineScene& /*scene*/) {
    // Phase 4: build TrackPlayers for the scene's tracks, prefetch, then publish
    // the new player list to the audio thread with a release store.
    (void)storage_;
    return core::notImplemented("AudioEngine::setScene (Phase 4)");
}

void AudioEngine::setTrackGain(core::TrackId /*id*/, float /*gain*/) noexcept {}
void AudioEngine::setTrackPan(core::TrackId /*id*/, float /*pan*/) noexcept {}
void AudioEngine::setTrackMuted(core::TrackId /*id*/, bool /*muted*/) noexcept {}
void AudioEngine::setTrackSoloed(core::TrackId /*id*/, bool /*soloed*/) noexcept {}

void AudioEngine::setMasterGain(float gain) noexcept { mixer_.setMasterGain(gain); }

void AudioEngine::renderAudio(std::span<float> output,
                              core::FrameCount frames,
                              core::ChannelCount channels) noexcept {
    // Realtime context. Everything below must stay allocation and lock free.
    const auto sampleCount =
        static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels);
    const std::size_t limit = std::min(sampleCount, output.size());
    std::fill_n(output.begin(), limit, 0.0F);

    // Phase 4: advance the transport, run the mixer, interleave into `output`,
    // and post PositionEvent/LevelEvent through eventQueue_ at UI rate.
    (void)transport_;
}

void AudioEngine::onXRun() noexcept {
    eventQueue_.post(events::NotificationEvent{core::ErrorCode::Timeout, true});
}

}  // namespace aw::audio
