#pragma once

#include <cstdint>

#include "aw/core/Result.hpp"

namespace aw::hal {

/// Receives rotary encoder motion.
///
/// The encoder's push switch is *not* here - it is `core::ButtonId::EncoderPush`
/// on `ButtonInterface`.  Splitting them keeps each interface single purpose and
/// lets the UI treat the push like any other button.
class EncoderListener {
public:
    virtual ~EncoderListener() = default;

    /// `delta` is in detents: positive clockwise, negative counter-clockwise.
    /// Backends coalesce quadrature steps into detents so the UI advances one
    /// list row per physical click regardless of encoder resolution.
    virtual void onEncoderTurned(std::int32_t delta, std::uint64_t timestampMicros) noexcept = 0;
};

/// A single rotary encoder - the device's primary navigation control.
///
/// Both a push (listener) and a pull (`consumeDelta`) API are offered because
/// the two consumers have different needs: the event system wants to be told,
/// while a fast scrub loop wants to drain accumulated motion once per frame
/// without processing a hundred queued events.
class EncoderInterface {
public:
    virtual ~EncoderInterface() = default;

    EncoderInterface(const EncoderInterface&) = delete;
    EncoderInterface& operator=(const EncoderInterface&) = delete;

    virtual void setListener(EncoderListener* listener) noexcept = 0;

    virtual core::Status start() = 0;
    virtual void stop() noexcept = 0;

    /// Returns motion accumulated since the last call and resets the accumulator.
    [[nodiscard]] virtual std::int32_t consumeDelta() noexcept = 0;

protected:
    EncoderInterface() = default;
};

}  // namespace aw::hal
