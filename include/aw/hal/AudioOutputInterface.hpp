#pragma once

#include <span>

#include "aw/core/AudioFormat.hpp"
#include "aw/core/Result.hpp"
#include "aw/core/Types.hpp"

namespace aw::hal {

/// Realtime audio producer, implemented by the audio engine.
///
/// Contract for implementers - this is the hard boundary of the project:
///   NO allocation, NO locks, NO file I/O, NO logging, NO exceptions.
/// Anything that can block belongs on the streaming thread, which hands data
/// over through a `core::RingBuffer`.
class AudioRenderCallback {
public:
    virtual ~AudioRenderCallback() = default;

    /// Fill `output` with exactly `frames * channels` samples.
    ///
    /// `output` is interleaved float in [-1, 1].  Float is used across this
    /// boundary even though the DAC is 24 bit integer: it keeps the mixer free
    /// of format concerns, and the int conversion is one cheap pass in the
    /// backend, which is also where dither will eventually live.
    virtual void renderAudio(std::span<float> output,
                             core::FrameCount frames,
                             core::ChannelCount channels) noexcept = 0;

    /// Reported when the backend detected a dropout.  Used for a UI indicator
    /// and for buffer size tuning; never for recovery logic.
    virtual void onXRun() noexcept {}
};

/// Audio playback device.
///
/// Pull model (the device asks for audio) rather than push (the app writes when
/// it feels like it).  ALSA natively prefers blocking writes, so the Pi backend
/// runs its own realtime thread and calls the callback from there; the ESP32-P4
/// I2S DMA is natively pull-shaped and maps directly.  Choosing pull means the
/// portable side has one timing model instead of two.
class AudioOutputInterface {
public:
    virtual ~AudioOutputInterface() = default;

    AudioOutputInterface(const AudioOutputInterface&) = delete;
    AudioOutputInterface& operator=(const AudioOutputInterface&) = delete;

    /// `bufferFrames` is the requested period size; the backend may round it and
    /// reports the accepted value through `format()`/`bufferFrames()`.
    /// The callback must outlive the open device.
    virtual core::Status open(const core::AudioFormat& format,
                              core::FrameCount bufferFrames,
                              AudioRenderCallback& callback) = 0;

    virtual core::Status start() = 0;
    virtual void stop() noexcept = 0;
    virtual void close() noexcept = 0;

    [[nodiscard]] virtual bool isRunning() const noexcept = 0;
    [[nodiscard]] virtual core::AudioFormat format() const noexcept = 0;
    [[nodiscard]] virtual core::FrameCount bufferFrames() const noexcept = 0;

    /// Output latency, for aligning the playhead with what is audible.
    [[nodiscard]] virtual core::FrameCount latencyFrames() const noexcept = 0;

protected:
    AudioOutputInterface() = default;
};

}  // namespace aw::hal
