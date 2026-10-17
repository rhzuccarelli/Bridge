#include "aw/platform/FallbackAudioOutput.hpp"

#include "aw/core/Logger.hpp"

namespace aw::platform {

FallbackAudioOutput::FallbackAudioOutput(std::unique_ptr<hal::AudioOutputInterface> primary,
                                         std::unique_ptr<hal::AudioOutputInterface> fallback)
    : primary_{std::move(primary)}, fallback_{std::move(fallback)} {}

FallbackAudioOutput::~FallbackAudioOutput() { close(); }

core::Status FallbackAudioOutput::open(const core::AudioFormat& format,
                                       core::FrameCount bufferFrames,
                                       hal::AudioRenderCallback& callback) {
    close();
    if (!primary_ || !fallback_) {
        return core::makeError(core::ErrorCode::InvalidState,
                               "audio fallback chain is incomplete");
    }
    requestedFormat_ = format;
    requestedBufferFrames_ = bufferFrames;
    callback_ = &callback;

    if (auto status = primary_->open(format, bufferFrames, callback); status) {
        selected_ = primary_.get();
        return core::success();
    } else {
        core::log::warning("audio", "primary output unavailable: " + status.error().describe() +
                                        "; using silent timing output");
        primary_->close();
    }

    if (auto status = fallback_->open(format, bufferFrames, callback); !status) {
        return status;
    }
    selected_ = fallback_.get();
    return core::success();
}

core::Status FallbackAudioOutput::start() {
    if (selected_ == nullptr) {
        return core::makeError(core::ErrorCode::InvalidState, "audio output not open");
    }
    return selected_->start();
}

core::Status FallbackAudioOutput::refresh() {
    if (selected_ == nullptr || callback_ == nullptr) return core::success();

    if (selected_ == primary_.get()) {
        if (primary_->isRunning()) return primary_->refresh();
        primary_->close();
        if (auto status = fallback_->open(requestedFormat_, requestedBufferFrames_, *callback_);
            !status) return status;
        selected_ = fallback_.get();
        core::log::warning("audio", "primary output disconnected; using silent timing output");
        return fallback_->start();
    }

    // Opening does not start callbacks, so it is safe to probe while the
    // silent timing backend continues driving the transport.
    if (auto status = primary_->open(requestedFormat_, requestedBufferFrames_, *callback_);
        !status) {
        primary_->close();
        return core::success();
    }
    fallback_->stop();
    fallback_->close();
    selected_ = primary_.get();
    if (auto status = primary_->start(); !status) {
        primary_->close();
        if (auto fallbackStatus = fallback_->open(requestedFormat_, requestedBufferFrames_,
                                                   *callback_);
            !fallbackStatus) return fallbackStatus;
        selected_ = fallback_.get();
        return fallback_->start();
    }
    core::log::info("audio", "primary output connected; monitoring is now active");
    return core::success();
}

void FallbackAudioOutput::stop() noexcept {
    if (selected_ != nullptr) {
        selected_->stop();
    }
}

void FallbackAudioOutput::close() noexcept {
    if (selected_ != nullptr) {
        selected_->close();
        selected_ = nullptr;
    }
    callback_ = nullptr;
}

bool FallbackAudioOutput::isRunning() const noexcept {
    return selected_ != nullptr && selected_->isRunning();
}

core::AudioFormat FallbackAudioOutput::format() const noexcept {
    return selected_ != nullptr ? selected_->format() : core::AudioFormat{};
}

core::FrameCount FallbackAudioOutput::bufferFrames() const noexcept {
    return selected_ != nullptr ? selected_->bufferFrames() : 0;
}

core::FrameCount FallbackAudioOutput::latencyFrames() const noexcept {
    return selected_ != nullptr ? selected_->latencyFrames() : 0;
}

bool FallbackAudioOutput::isUsingFallback() const noexcept {
    return selected_ != nullptr && selected_ == fallback_.get();
}

}  // namespace aw::platform
