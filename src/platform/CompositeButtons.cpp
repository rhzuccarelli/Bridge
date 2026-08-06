#include "aw/platform/CompositeButtons.hpp"

#include <algorithm>

#include "aw/core/Logger.hpp"

namespace aw::platform {
namespace {
constexpr std::string_view kModule = "platform";
}

CompositeButtons::~CompositeButtons() { stop(); }

void CompositeButtons::addSource(std::unique_ptr<hal::ButtonInterface> source) {
    if (source) {
        sources_.push_back(std::move(source));
    }
}

void CompositeButtons::setListener(hal::ButtonListener* listener) noexcept {
    // Every source posts to the same listener, which is what makes the merge
    // free: there is no queue here and no ordering to arbitrate.
    for (auto& source : sources_) {
        source->setListener(listener);
    }
}

core::Status CompositeButtons::start() {
    startedCount_ = 0;

    for (auto& source : sources_) {
        if (auto status = source->start(); status) {
            ++startedCount_;
        } else {
            core::log::warning(kModule, "button source unavailable: " +
                                            status.error().describe());
        }
    }

    if (startedCount_ == 0) {
        return core::makeError(core::ErrorCode::DeviceNotConnected,
                               "no button source could be started");
    }
    return core::success();
}

void CompositeButtons::stop() noexcept {
    for (auto it = sources_.rbegin(); it != sources_.rend(); ++it) {
        (*it)->stop();
    }
    startedCount_ = 0;
}

bool CompositeButtons::isPressed(core::ButtonId id) const noexcept {
    return std::any_of(sources_.begin(), sources_.end(),
                       [id](const std::unique_ptr<hal::ButtonInterface>& source) {
                           return source->isPressed(id);
                       });
}

}  // namespace aw::platform
