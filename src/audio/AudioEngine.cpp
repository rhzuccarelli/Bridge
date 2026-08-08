#include "aw/audio/AudioEngine.hpp"

#include "aw/core/RingBuffer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

namespace aw::audio {
namespace {

float clampSample(float value) noexcept { return std::clamp(value, -1.0F, 1.0F); }

struct EngineSceneState {
    std::vector<std::unique_ptr<TrackPlayer>> players;
    std::vector<TrackPlayer*> activePlayers;
    std::vector<core::TrackId> trackIds;
    core::FrameCount lengthFrames{0};
    core::FrameRange loopRange{};
    bool loopEnabled{false};
};

bool sameSourcePlacement(const EngineTrack& first, const EngineTrack& second) noexcept {
    return first.audioFile == second.audioFile && first.startFrame == second.startFrame &&
           first.sourceRange.start == second.sourceRange.start &&
           first.sourceRange.length == second.sourceRange.length;
}

enum class MixCommandKind : std::uint8_t { Gain, Pan, Mute, Solo, MasterGain };

struct MixCommand {
    MixCommandKind kind{MixCommandKind::Gain};
    core::TrackId trackId{core::kInvalidTrackId};
    float value{0.0F};
};

static_assert(std::is_trivially_copyable_v<MixCommand>);

}  // namespace

struct AudioEngine::Impl {
    std::atomic<EngineSceneState*> pendingScene{nullptr};
    std::atomic<EngineSceneState*> activeScene{nullptr};
    core::RingBuffer<EngineSceneState*, 64> retiredScenes;
    core::RingBuffer<MixCommand, 64> mixCommands;
    AudioBuffer mixBuffer;
    std::jthread streamingThread;
    std::atomic<bool> running{false};
    events::TransportState lastTransportState{events::TransportState::Stopped};
    bool hasPostedTransportState{false};
    std::uint64_t lastSeekRevision{0};
    core::FrameCount framesUntilTelemetry{0};
};

AudioEngine::AudioEngine(hal::AudioOutputInterface& output,
                         hal::StorageInterface& storage,
                         events::EventQueue& eventQueue)
    : output_{output},
      storage_{storage},
      eventQueue_{eventQueue},
      impl_{std::make_unique<Impl>()} {}

AudioEngine::~AudioEngine() {
    stop();
    delete impl_->pendingScene.exchange(nullptr, std::memory_order_acq_rel);
    delete impl_->activeScene.exchange(nullptr, std::memory_order_acq_rel);
    while (const auto retired = impl_->retiredScenes.pop()) {
        delete *retired;
    }
}

core::Status AudioEngine::start(core::FrameCount bufferFrames) {
    if (impl_->running.load(std::memory_order_acquire)) {
        return core::makeError(core::ErrorCode::InvalidState, "AudioEngine already started");
    }
    if (bufferFrames <= 0) {
        return core::makeError(core::ErrorCode::InvalidArgument, "invalid audio buffer size");
    }

    mixer_.prepare(core::kNativeFormat.channels, bufferFrames);
    impl_->mixBuffer = AudioBuffer{core::kNativeFormat.channels, bufferFrames};
    impl_->hasPostedTransportState = false;

    if (auto status = output_.open(core::kNativeFormat, bufferFrames, *this); !status) {
        return status;
    }

    impl_->streamingThread = std::jthread{[this](std::stop_token stopToken) {
        while (!stopToken.stop_requested()) {
            bool produced = false;
            EngineSceneState* scene = impl_->activeScene.load(std::memory_order_acquire);
            if (scene != nullptr) {
                for (const auto& player : scene->players) {
                    produced = player->stream() || produced;
                }
            }

            // Only this thread reclaims scenes. If it loaded the old scene just
            // before the audio callback retired it, reclamation happens here
            // after the iteration has finished using that pointer.
            while (const auto retired = impl_->retiredScenes.pop()) {
                delete *retired;
            }

            if (!produced) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }
    }};

    if (auto status = output_.start(); !status) {
        impl_->streamingThread.request_stop();
        impl_->streamingThread.join();
        output_.close();
        return status;
    }

    impl_->running.store(true, std::memory_order_release);
    return core::success();
}

void AudioEngine::stop() noexcept {
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    output_.stop();
    output_.close();
    impl_->streamingThread.request_stop();
    if (impl_->streamingThread.joinable()) {
        impl_->streamingThread.join();
    }
}

bool AudioEngine::isRunning() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

core::Status AudioEngine::setScene(const EngineScene& scene) {
    if (scene.sampleRate != core::kNativeFormat.sampleRate) {
        return core::makeError(core::ErrorCode::UnsupportedFormat,
                               "project sample rate is not the native 48 kHz");
    }

    auto prepared = std::make_unique<EngineSceneState>();
    prepared->players.reserve(scene.tracks.size());
    prepared->activePlayers.reserve(scene.tracks.size());
    prepared->trackIds.reserve(scene.tracks.size());
    prepared->lengthFrames = scene.lengthFrames;
    prepared->loopRange = scene.loopRange;
    prepared->loopEnabled = scene.loopEnabled;

    std::vector<std::size_t> sourceRepresentatives;
    sourceRepresentatives.reserve(scene.tracks.size());

    for (std::size_t trackIndex = 0; trackIndex < scene.tracks.size(); ++trackIndex) {
        const EngineTrack& track = scene.tracks[trackIndex];
        auto player = std::make_unique<TrackPlayer>();
        std::optional<std::size_t> sharedRepresentative;
        for (const std::size_t candidate : sourceRepresentatives) {
            if (sameSourcePlacement(track, scene.tracks[candidate])) {
                sharedRepresentative = candidate;
                break;
            }
        }
        if (sharedRepresentative) {
            if (auto status = player->shareSource(*prepared->players[*sharedRepresentative],
                                                  track.sourceChannel,
                                                  track.sourceChannelCount,
                                                  track.destinationChannel,
                                                  track.destinationChannelCount);
                !status) {
                return status;
            }
        } else {
            if (auto status = player->setSource(storage_, track.audioFile, track.sourceChannel,
                                                track.sourceChannelCount, track.destinationChannel,
                                                track.destinationChannelCount);
                !status) {
                return status;
            }
            if (auto status = player->prefetch(track.startFrame, track.sourceRange); !status) {
                return status;
            }
            sourceRepresentatives.push_back(trackIndex);
        }
        player->mix().gain.store(track.gain, std::memory_order_relaxed);
        player->mix().pan.store(track.pan, std::memory_order_relaxed);
        player->mix().muted.store(track.muted, std::memory_order_relaxed);
        player->mix().soloed.store(track.soloed, std::memory_order_relaxed);
        player->setLoop(scene.loopRange, scene.loopEnabled);
        prepared->trackIds.push_back(track.id);
        prepared->activePlayers.push_back(player.get());
        prepared->players.push_back(std::move(player));
    }

    EngineSceneState* replaced =
        impl_->pendingScene.exchange(prepared.release(), std::memory_order_acq_rel);
    delete replaced;  // Never became visible to the audio thread.
    return core::success();
}

void AudioEngine::setTrackGain(core::TrackId id, float gain) noexcept {
    (void)impl_->mixCommands.push(MixCommand{MixCommandKind::Gain, id, gain});
}

void AudioEngine::setTrackPan(core::TrackId id, float pan) noexcept {
    (void)impl_->mixCommands.push(MixCommand{MixCommandKind::Pan, id, pan});
}

void AudioEngine::setTrackMuted(core::TrackId id, bool muted) noexcept {
    (void)impl_->mixCommands.push(MixCommand{MixCommandKind::Mute, id, muted ? 1.0F : 0.0F});
}

void AudioEngine::setTrackSoloed(core::TrackId id, bool soloed) noexcept {
    (void)impl_->mixCommands.push(MixCommand{MixCommandKind::Solo, id, soloed ? 1.0F : 0.0F});
}

void AudioEngine::setMasterGain(float gain) noexcept {
    (void)impl_->mixCommands.push(
        MixCommand{MixCommandKind::MasterGain, core::kInvalidTrackId, gain});
}

void AudioEngine::renderAudio(std::span<float> output,
                              core::FrameCount frames,
                              core::ChannelCount channels) noexcept {
    const auto requestedSamples =
        static_cast<std::size_t>(std::max<core::FrameCount>(frames, 0)) * channels;
    std::fill_n(output.begin(), std::min(requestedSamples, output.size()), 0.0F);

    if (EngineSceneState* incoming =
            impl_->pendingScene.exchange(nullptr, std::memory_order_acq_rel);
        incoming != nullptr) {
        EngineSceneState* old = impl_->activeScene.exchange(incoming, std::memory_order_acq_rel);
        transport_.setLoop(incoming->loopRange, incoming->loopEnabled);
        if (old != nullptr && !impl_->retiredScenes.push(old)) {
            // Replacing more than 63 scenes before the streaming thread runs is
            // pathological. Leaking is safer than deleting on the audio thread.
            eventQueue_.post(events::NotificationEvent{core::ErrorCode::OutOfMemory, true});
        }
    }

    EngineSceneState* scene = impl_->activeScene.load(std::memory_order_acquire);
    while (const auto command = impl_->mixCommands.pop()) {
        if (command->kind == MixCommandKind::MasterGain) {
            mixer_.setMasterGain(command->value);
            continue;
        }
        if (scene == nullptr) {
            continue;
        }
        for (std::size_t i = 0; i < scene->trackIds.size(); ++i) {
            if (scene->trackIds[i] != command->trackId) {
                continue;
            }
            TrackMixState& mix = scene->players[i]->mix();
            switch (command->kind) {
                case MixCommandKind::Gain:
                    mix.gain.store(command->value, std::memory_order_relaxed);
                    break;
                case MixCommandKind::Pan:
                    mix.pan.store(command->value, std::memory_order_relaxed);
                    break;
                case MixCommandKind::Mute:
                    mix.muted.store(command->value != 0.0F, std::memory_order_relaxed);
                    break;
                case MixCommandKind::Solo:
                    mix.soloed.store(command->value != 0.0F, std::memory_order_relaxed);
                    break;
                case MixCommandKind::MasterGain: break;
            }
        }
    }

    const core::FrameCount renderFrames = std::clamp<core::FrameCount>(
        frames, 0, impl_->mixBuffer.capacityFrames());
    core::FrameIndex blockStart = transport_.advance(0);
    const std::uint64_t seekRevision = transport_.seekRevision();
    if (scene != nullptr && seekRevision != impl_->lastSeekRevision) {
        for (const auto& player : scene->players) {
            player->requestStreamPosition(blockStart);
        }
        impl_->lastSeekRevision = seekRevision;
    }
    const auto state = transport_.state();
    if (scene != nullptr && renderFrames > 0 &&
        (state == events::TransportState::Playing ||
         state == events::TransportState::Recording)) {
        core::FrameCount rendered = 0;
        while (rendered < renderFrames) {
            core::FrameCount segment = renderFrames - rendered;
            if (transport_.isLoopEnabled()) {
                const core::FrameRange loop = transport_.loopRange();
                if (loop.length > 0 && blockStart < loop.end() && blockStart + segment > loop.end()) {
                    segment = loop.end() - blockStart;
                }
            }
            if (segment <= 0) {
                break;
            }

            mixer_.render(scene->activePlayers, impl_->mixBuffer, blockStart, segment);
            const core::ChannelCount renderChannels =
                std::min<core::ChannelCount>(channels, impl_->mixBuffer.channelCount());
            for (core::FrameCount frame = 0; frame < segment; ++frame) {
                for (core::ChannelCount channel = 0; channel < renderChannels; ++channel) {
                    const std::size_t outputIndex =
                        static_cast<std::size_t>(rendered + frame) * channels + channel;
                    if (outputIndex < output.size()) {
                        output[outputIndex] = clampSample(
                            impl_->mixBuffer.channel(channel)[static_cast<std::size_t>(frame)]);
                    }
                }
            }
            rendered += segment;
            (void)transport_.advance(segment);
            blockStart = transport_.advance(0);
        }
    }

    const auto transportState = transport_.state();
    const auto position = transport_.position();
    if (!impl_->hasPostedTransportState || transportState != impl_->lastTransportState) {
        eventQueue_.post(events::TransportEvent{transportState, position});
        impl_->lastTransportState = transportState;
        impl_->hasPostedTransportState = true;
    }

    const bool postTelemetry = impl_->framesUntilTelemetry <= 0;
    impl_->framesUntilTelemetry -= renderFrames;
    if (postTelemetry) {
        impl_->framesUntilTelemetry =
            static_cast<core::FrameCount>(core::kNativeFormat.sampleRate / 30U);
    }

    if (scene != nullptr && postTelemetry) {
        bool streamUnderrun = false;
        for (std::size_t i = 0; i < scene->trackIds.size(); ++i) {
            streamUnderrun = scene->players[i]->consumeUnderrunFlag() || streamUnderrun;
            bool seen = false;
            for (std::size_t prior = 0; prior < i; ++prior) {
                seen = seen || scene->trackIds[prior] == scene->trackIds[i];
            }
            if (seen) {
                continue;
            }
            float peak = 0.0F;
            for (std::size_t candidate = i; candidate < scene->trackIds.size(); ++candidate) {
                if (scene->trackIds[candidate] == scene->trackIds[i]) {
                    peak = std::max(peak, scene->players[candidate]->lastPeak());
                }
            }
            eventQueue_.post(events::LevelEvent{scene->trackIds[i], peak});
        }
        if (streamUnderrun) {
            eventQueue_.post(events::NotificationEvent{core::ErrorCode::Timeout, true});
        }
    }
    if (postTelemetry) {
        eventQueue_.post(events::PositionEvent{position});
    }
}

void AudioEngine::onXRun() noexcept {
    eventQueue_.post(events::NotificationEvent{core::ErrorCode::Timeout, true});
}

}  // namespace aw::audio
