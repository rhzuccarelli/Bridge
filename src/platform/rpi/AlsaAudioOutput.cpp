#include "aw/platform/rpi/AlsaAudioOutput.hpp"

#include <string>

// Phase 5 replaces these stubs with the real ALSA implementation.  The class
// compiles today without libasound so the Pi target can be built and wired up
// before the driver exists.

namespace aw::platform::rpi {

struct AlsaAudioOutput::Impl {
    std::string deviceName;
    core::AudioFormat format{core::kNativeFormat};
    core::FrameCount bufferFrames{256};
    hal::AudioRenderCallback* callback{nullptr};
    bool running{false};
};

AlsaAudioOutput::AlsaAudioOutput(std::string deviceName)
    : impl_{std::make_unique<Impl>()} {
    impl_->deviceName = std::move(deviceName);
}

AlsaAudioOutput::~AlsaAudioOutput() = default;

core::Status AlsaAudioOutput::open(const core::AudioFormat& format,
                                   core::FrameCount bufferFrames,
                                   hal::AudioRenderCallback& callback) {
    impl_->format = format;
    impl_->bufferFrames = bufferFrames;
    impl_->callback = &callback;
    return core::notImplemented("AlsaAudioOutput::open (Phase 5)");
}

core::Status AlsaAudioOutput::start() {
    return core::notImplemented("AlsaAudioOutput::start (Phase 5)");
}

void AlsaAudioOutput::stop() noexcept { impl_->running = false; }

void AlsaAudioOutput::close() noexcept {
    stop();
    impl_->callback = nullptr;
}

bool AlsaAudioOutput::isRunning() const noexcept { return impl_->running; }
core::AudioFormat AlsaAudioOutput::format() const noexcept { return impl_->format; }
core::FrameCount AlsaAudioOutput::bufferFrames() const noexcept { return impl_->bufferFrames; }
core::FrameCount AlsaAudioOutput::latencyFrames() const noexcept {
    return impl_->bufferFrames * 2;  // Two periods; confirmed against ALSA in Phase 5.
}

}  // namespace aw::platform::rpi
