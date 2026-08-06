#include "aw/platform/NullAudioOutput.hpp"

#include <chrono>

namespace aw::platform {

NullAudioOutput::~NullAudioOutput() { close(); }

core::Status NullAudioOutput::open(const core::AudioFormat& format,
                                   core::FrameCount bufferFrames,
                                   hal::AudioRenderCallback& callback) {
    format_ = format;
    bufferFrames_ = bufferFrames;
    callback_ = &callback;
    scratch_.assign(static_cast<std::size_t>(bufferFrames) * format.channels, 0.0F);
    return core::success();
}

core::Status NullAudioOutput::start() {
    if (callback_ == nullptr) {
        return core::makeError(core::ErrorCode::InvalidState, "audio output not open");
    }
    if (running_.exchange(true)) {
        return core::success();
    }
    thread_ = std::thread{[this] { run(); }};
    return core::success();
}

void NullAudioOutput::stop() noexcept {
    if (running_.exchange(false) && thread_.joinable()) {
        thread_.join();
    }
}

void NullAudioOutput::close() noexcept {
    stop();
    callback_ = nullptr;
    scratch_.clear();
}

void NullAudioOutput::run() {
    const auto period = std::chrono::microseconds{
        (static_cast<std::int64_t>(bufferFrames_) * 1'000'000) /
        static_cast<std::int64_t>(format_.sampleRate)};
    auto next = std::chrono::steady_clock::now();

    while (running_.load()) {
        next += period;
        callback_->renderAudio(scratch_, bufferFrames_, format_.channels);
        std::this_thread::sleep_until(next);
    }
}

}  // namespace aw::platform
