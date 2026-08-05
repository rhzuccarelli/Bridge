#pragma once

#include <span>

#include "aw/core/AudioFormat.hpp"
#include "aw/core/Result.hpp"
#include "aw/core/Types.hpp"

namespace aw::hal {

/// Realtime audio consumer, implemented by the audio engine's recorder.
///
/// Same hard contract as `AudioRenderCallback`: no allocation, no locks, no I/O.
class AudioCaptureCallback {
public:
    virtual ~AudioCaptureCallback() = default;

    /// `input` holds `frames * channels` interleaved float samples in [-1, 1].
    /// The span is only valid for the duration of the call.
    virtual void captureAudio(std::span<const float> input,
                              core::FrameCount frames,
                              core::ChannelCount channels) noexcept = 0;

    virtual void onOverrun() noexcept {}
};

/// Audio capture device.
///
/// Declared in Phase 1 although recording is Phase 4+ work, because its
/// existence changes the engine's shape: a device that will eventually do
/// punch-in needs a single clock driving capture and playback together.
/// Retrofitting that later is a rewrite; reserving the seat now is free.
class AudioInputInterface {
public:
    virtual ~AudioInputInterface() = default;

    AudioInputInterface(const AudioInputInterface&) = delete;
    AudioInputInterface& operator=(const AudioInputInterface&) = delete;

    virtual core::Status open(const core::AudioFormat& format,
                              core::FrameCount bufferFrames,
                              AudioCaptureCallback& callback) = 0;

    virtual core::Status start() = 0;
    virtual void stop() noexcept = 0;
    virtual void close() noexcept = 0;

    [[nodiscard]] virtual bool isRunning() const noexcept = 0;
    [[nodiscard]] virtual core::AudioFormat format() const noexcept = 0;
    [[nodiscard]] virtual core::FrameCount latencyFrames() const noexcept = 0;

protected:
    AudioInputInterface() = default;
};

}  // namespace aw::hal
