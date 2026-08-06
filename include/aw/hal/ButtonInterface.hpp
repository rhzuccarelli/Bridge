#pragma once

#include <cstdint>

#include "aw/core/Controls.hpp"
#include "aw/core/Result.hpp"

namespace aw::hal {

/// Receives debounced button transitions.
///
/// Implemented by the event layer, not by UI code: the listener's only job is
/// to turn a hardware transition into an `events::Event` and post it.  Nothing
/// downstream of this interface knows a GPIO exists.
class ButtonListener {
public:
    virtual ~ButtonListener() = default;

    /// Called from the backend's input thread (or ISR context on the ESP32).
    /// Implementations must be non-blocking and must not allocate.
    virtual void onButtonEvent(core::ButtonId id,
                               core::ButtonAction action,
                               std::uint64_t timestampMicros) noexcept = 0;
};

/// The physical button array.
///
/// Debounce, long-press and auto-repeat timing live *inside* the backend, on
/// purpose.  On the Pi that means a poll thread; on the ESP32-P4 it will mean a
/// GPIO ISR plus a timer.  Both produce the same clean event stream, so the
/// application's notion of "held for half a second" ports unchanged.
class ButtonInterface {
public:
    virtual ~ButtonInterface() = default;

    ButtonInterface(const ButtonInterface&) = delete;
    ButtonInterface& operator=(const ButtonInterface&) = delete;

    /// Must be called before `start()`.  A null listener detaches.
    virtual void setListener(ButtonListener* listener) noexcept = 0;

    /// Begins delivering events.
    virtual core::Status start() = 0;

    /// Stops delivery.  After it returns, the listener is guaranteed not to be
    /// called again, so it can safely be destroyed.
    virtual void stop() noexcept = 0;

    /// Current debounced level.  For "is Shift held" style modifiers, where
    /// polling is simpler than tracking edges.
    [[nodiscard]] virtual bool isPressed(core::ButtonId id) const noexcept = 0;

protected:
    ButtonInterface() = default;
};

}  // namespace aw::hal
