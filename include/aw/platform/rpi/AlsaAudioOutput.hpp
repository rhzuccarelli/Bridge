#pragma once

#include <memory>

#include "aw/hal/AudioOutputInterface.hpp"

namespace aw::platform::rpi {

/// ALSA playback backend for the Raspberry Pi 5.  (Phase 5.)
///
/// Design notes, decided now so the interface above does not have to change
/// later:
///  - Opens the PCM in `S24_3LE` at 48 kHz stereo, matching `kNativeFormat`, so
///    there is no rate or format conversion anywhere in the chain.  If the
///    device cannot do it, opening fails loudly rather than silently resampling.
///  - Runs its own thread at `SCHED_FIFO` and calls the render callback from
///    there, converting float to packed 24 bit on the way out.  ALSA's blocking
///    write model becomes the pull model the portable side expects.
///  - Period and buffer sizes are explicit, not left to ALSA's defaults: two
///    periods of 256 frames is ~10 ms of latency, which is what makes the
///    transport feel like hardware.
///  - `snd_pcm_recover` handles xruns and reports them upward; it never hides one.
class AlsaAudioOutput final : public hal::AudioOutputInterface {
public:
    /// `deviceName` is an ALSA PCM name, e.g. "hw:0,0" or "default".
    explicit AlsaAudioOutput(std::string deviceName);
    ~AlsaAudioOutput() override;

    core::Status open(const core::AudioFormat& format,
                      core::FrameCount bufferFrames,
                      hal::AudioRenderCallback& callback) override;
    core::Status start() override;
    void stop() noexcept override;
    void close() noexcept override;

    [[nodiscard]] bool isRunning() const noexcept override;
    [[nodiscard]] core::AudioFormat format() const noexcept override;
    [[nodiscard]] core::FrameCount bufferFrames() const noexcept override;
    [[nodiscard]] core::FrameCount latencyFrames() const noexcept override;

private:
    struct Impl;  ///< Hides every ALSA type; no <alsa/asoundlib.h> in this header.
    std::unique_ptr<Impl> impl_;
};

}  // namespace aw::platform::rpi
