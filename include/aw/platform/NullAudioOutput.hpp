#pragma once

#include <atomic>
#include <thread>
#include <vector>

#include "aw/hal/AudioOutputInterface.hpp"

namespace aw::platform {

/// A silent audio output that still drives the render callback on a real clock.
///
/// It calls `renderAudio` at the true block rate and throws the samples away.
/// That is deliberately *not* a no-op: because the callback runs on a wall
/// clock, the transport advances, meters move, and the streaming logic is
/// exercised end to end - so the whole app runs and behaves correctly with no
/// sound card at all. A callback that overruns its deadline shows up here just
/// as it would on real hardware.
///
/// It is the development audio backend for both the simulator and, until the
/// Phase 5 ALSA driver exists, the Raspberry Pi. When ALSA lands it becomes the
/// fallback the Pi uses when no usable playback device is present, rather than
/// letting the whole device fail to start over a missing speaker.
class NullAudioOutput final : public hal::AudioOutputInterface {
public:
    ~NullAudioOutput() override;

    core::Status open(const core::AudioFormat& format,
                      core::FrameCount bufferFrames,
                      hal::AudioRenderCallback& callback) override;
    core::Status start() override;
    void stop() noexcept override;
    void close() noexcept override;

    [[nodiscard]] bool isRunning() const noexcept override { return running_.load(); }
    [[nodiscard]] core::AudioFormat format() const noexcept override { return format_; }
    [[nodiscard]] core::FrameCount bufferFrames() const noexcept override { return bufferFrames_; }
    [[nodiscard]] core::FrameCount latencyFrames() const noexcept override { return bufferFrames_; }

private:
    void run();

    core::AudioFormat format_{};
    core::FrameCount bufferFrames_{0};
    hal::AudioRenderCallback* callback_{nullptr};
    std::vector<float> scratch_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace aw::platform
