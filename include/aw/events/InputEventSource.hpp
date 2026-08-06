#pragma once

#include "aw/events/EventQueue.hpp"
#include "aw/hal/ButtonInterface.hpp"
#include "aw/hal/EncoderInterface.hpp"

namespace aw::events {

/// Adapts the HAL's input callbacks into the event queue.
///
/// This class is the entire reason no GPIO type ever reaches the UI: it is the
/// only implementation of `hal::ButtonListener` and `hal::EncoderListener` in
/// the portable codebase, and all it does is translate and post.  Its callbacks
/// run on the backend's input thread, so the only thing they are allowed to do
/// is a wait free push.
class InputEventSource final : public hal::ButtonListener, public hal::EncoderListener {
public:
    explicit InputEventSource(EventQueue& queue) noexcept : queue_{queue} {}

    // hal::ButtonListener
    void onButtonEvent(core::ButtonId id,
                       core::ButtonAction action,
                       std::uint64_t timestampMicros) noexcept override;

    // hal::EncoderListener
    void onEncoderTurned(std::int32_t delta, std::uint64_t timestampMicros) noexcept override;

private:
    EventQueue& queue_;
};

}  // namespace aw::events
