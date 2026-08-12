#pragma once

#include <memory>
#include <utility>

#include "aw/hal/AudioOutputInterface.hpp"

namespace aw::platform {

/// Selects a real output when it opens successfully and otherwise falls back
/// to a timing-compatible silent output. Selection happens once per open; all
/// later calls delegate to that same backend.
class FallbackAudioOutput final : public hal::AudioOutputInterface {
public:
    FallbackAudioOutput(std::unique_ptr<hal::AudioOutputInterface> primary,
                        std::unique_ptr<hal::AudioOutputInterface> fallback);
    ~FallbackAudioOutput() override;

    core::Status open(const core::AudioFormat& format,
                      core::FrameCount bufferFrames,
                      hal::AudioRenderCallback& callback) override;
    core::Status start() override;
    core::Status refresh() override;
    void stop() noexcept override;
    void close() noexcept override;

    [[nodiscard]] bool isRunning() const noexcept override;
    [[nodiscard]] core::AudioFormat format() const noexcept override;
    [[nodiscard]] core::FrameCount bufferFrames() const noexcept override;
    [[nodiscard]] core::FrameCount latencyFrames() const noexcept override;
    [[nodiscard]] bool isUsingFallback() const noexcept;

private:
    std::unique_ptr<hal::AudioOutputInterface> primary_;
    std::unique_ptr<hal::AudioOutputInterface> fallback_;
    hal::AudioOutputInterface* selected_{nullptr};
    core::AudioFormat requestedFormat_{};
    core::FrameCount requestedBufferFrames_{0};
    hal::AudioRenderCallback* callback_{nullptr};
};

}  // namespace aw::platform
