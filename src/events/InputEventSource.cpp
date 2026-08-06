#include "aw/events/InputEventSource.hpp"

namespace aw::events {

void InputEventSource::onButtonEvent(core::ButtonId id,
                                     core::ButtonAction action,
                                     std::uint64_t timestampMicros) noexcept {
    queue_.post(ButtonEvent{id, action, timestampMicros});
}

void InputEventSource::onEncoderTurned(std::int32_t delta,
                                       std::uint64_t timestampMicros) noexcept {
    queue_.post(EncoderEvent{delta, timestampMicros});
}

}  // namespace aw::events
