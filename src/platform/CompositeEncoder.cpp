#include "aw/platform/CompositeEncoder.hpp"

#include "aw/core/Logger.hpp"

namespace aw::platform {
namespace {
constexpr std::string_view kModule = "platform";
}

CompositeEncoder::~CompositeEncoder() { stop(); }

void CompositeEncoder::addSource(std::unique_ptr<hal::EncoderInterface> source) {
    if (source) {
        sources_.push_back(std::move(source));
    }
}

void CompositeEncoder::setListener(hal::EncoderListener* listener) noexcept {
    for (auto& source : sources_) {
        source->setListener(listener);
    }
}

core::Status CompositeEncoder::start() {
    startedCount_ = 0;
    for (auto& source : sources_) {
        if (auto status = source->start(); status) {
            ++startedCount_;
        } else {
            core::log::warning(kModule, "encoder source unavailable: " +
                                            status.error().describe());
        }
    }
    if (startedCount_ == 0) {
        return core::makeError(core::ErrorCode::DeviceNotConnected,
                               "no encoder source could be started");
    }
    return core::success();
}

void CompositeEncoder::stop() noexcept {
    for (auto it = sources_.rbegin(); it != sources_.rend(); ++it) {
        (*it)->stop();
    }
    startedCount_ = 0;
}

std::int32_t CompositeEncoder::consumeDelta() noexcept {
    std::int32_t total = 0;
    for (auto& source : sources_) {
        total += source->consumeDelta();
    }
    return total;
}

}  // namespace aw::platform
