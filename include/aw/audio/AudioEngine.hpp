#pragma once

#include <memory>
#include <vector>

#include "aw/audio/AudioBuffer.hpp"
#include "aw/audio/Mixer.hpp"
#include "aw/audio/TrackPlayer.hpp"
#include "aw/audio/Transport.hpp"
#include "aw/core/Result.hpp"
#include "aw/events/EventQueue.hpp"
#include "aw/hal/AudioOutputInterface.hpp"
#include "aw/hal/StorageInterface.hpp"

namespace aw::audio {

/// Immutable description of what the engine should play.
///
/// This is the *only* thing that crosses from the project layer into the audio
/// layer, and it is a plain value.  The engine never sees `project::Project`,
/// never holds a pointer into it, and therefore cannot be broken by an edit
/// happening on the UI thread.  When the project changes, the project layer
/// builds a new `EngineScene` and hands it over; the engine swaps it in at a
/// block boundary.
///
/// This is the single most important boundary in the design: it is what lets
/// the audio engine be unit tested with no project code linked, and what will
/// let the ESP32 port keep the engine byte-for-byte identical.
struct EngineTrack {
    core::TrackId id{core::kInvalidTrackId};
    core::Path audioFile;      ///< Absolute path to a local WAV. Never an MTP object.
    core::FrameIndex startFrame{0};  ///< Where the file begins on the timeline.
    core::FrameRange sourceRange{};  ///< Which part of the file this clip plays.
    float gain{1.0F};
    float pan{0.0F};
    bool muted{false};
    bool soloed{false};
    core::ChannelCount sourceChannel{0};  ///< Zero-based first source channel.
    core::ChannelCount sourceChannelCount{2};  ///< Mono (1) or stereo pair (2).
    core::ChannelCount destinationChannel{0};  ///< 0 = L, 1 = R.
    core::ChannelCount destinationChannelCount{2};
};

struct EngineScene {
    core::SampleRate sampleRate{48000};
    std::vector<EngineTrack> tracks;
    core::FrameCount lengthFrames{0};
    core::FrameRange loopRange{};
    bool loopEnabled{false};
};

/// Realtime multitrack playback.
///
/// Phase 4 implements this.  Threading model, fixed now because everything else
/// depends on it:
///
///   audio thread      - `renderAudio()`. No allocation, no locks, no I/O.
///   streaming thread  - reads WAV data ahead into per-source ring buffers.
///   control thread    - the main loop. Calls `setScene`, transport methods.
///
/// The engine posts `TransportEvent`, `PositionEvent` and `LevelEvent` into an
/// `events::EventQueue`; it never calls the UI back directly.
class AudioEngine final : public hal::AudioRenderCallback {
public:
    AudioEngine(hal::AudioOutputInterface& output,
                hal::StorageInterface& storage,
                events::EventQueue& eventQueue);
    ~AudioEngine() override;

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    /// Opens the audio device and starts the streaming thread.
    [[nodiscard]] core::Status start(core::FrameCount bufferFrames);
    void stop() noexcept;
    [[nodiscard]] bool isRunning() const noexcept;

    /// Replaces what is playing.  Safe to call while running; takes effect at
    /// the next block boundary.  Blocking (it opens files), so control thread only.
    [[nodiscard]] core::Status setScene(const EngineScene& scene);

    [[nodiscard]] Transport& transport() noexcept { return transport_; }
    [[nodiscard]] const Transport& transport() const noexcept { return transport_; }

    /// Live mix tweaks that must not rebuild the scene (encoder turning a gain).
    void setTrackGain(core::TrackId id, float gain) noexcept;
    void setTrackPan(core::TrackId id, float pan) noexcept;
    void setTrackMuted(core::TrackId id, bool muted) noexcept;
    void setTrackSoloed(core::TrackId id, bool soloed) noexcept;
    void setMasterGain(float gain) noexcept;

    // hal::AudioRenderCallback
    void renderAudio(std::span<float> output,
                     core::FrameCount frames,
                     core::ChannelCount channels) noexcept override;
    void onXRun() noexcept override;

private:
    struct Impl;

    hal::AudioOutputInterface& output_;
    hal::StorageInterface& storage_;
    events::EventQueue& eventQueue_;
    Transport transport_;
    Mixer mixer_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aw::audio
