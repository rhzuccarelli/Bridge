#pragma once

#include <array>
#include <memory>

#include "aw/core/Controls.hpp"
#include "aw/hal/ButtonInterface.hpp"
#include "aw/hal/EncoderInterface.hpp"

namespace aw::platform::esp32p4 {

/// Which GPIO each control sits on; -1 means not wired.
struct EspPinMap {
    std::array<int, core::kButtonCount> buttonLines{};
    int encoderLineA{-1};  ///< KY-040 CLK.
    int encoderLineB{-1};  ///< KY-040 DT.

    /// The Waveshare ESP32-P4-Pico prototype: a KY-040 with CLK on GPIO16, DT
    /// on GPIO15 and SW on GPIO17, + on 3.3 V. Found with a pin-finder on the
    /// bench; clockwise turns make CLK lead DT.
    [[nodiscard]] static EspPinMap picoPrototype() noexcept;
};

/// Buttons on GPIO: active low with internal pull-ups, sampled at 1 kHz by a
/// high-priority task and run through the shared `ButtonStateMachine`, so
/// debounce, long press and repeat behave exactly as on the Pi.
///
/// Polling rather than GPIO interrupts: 1 ms is far below the 20 ms debounce
/// window, it costs a few microseconds per tick, and the listener is called
/// from task context, never from an ISR.
class EspGpioButtons final : public hal::ButtonInterface {
public:
    explicit EspGpioButtons(EspPinMap pins);
    ~EspGpioButtons() override;

    void setListener(hal::ButtonListener* listener) noexcept override;
    core::Status start() override;
    void stop() noexcept override;
    [[nodiscard]] bool isPressed(core::ButtonId id) const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// A quadrature encoder on two GPIOs, sampled at 1 kHz through the shared
/// `QuadratureDecoder`. A KY-040 detent spans several milliseconds, measured at
/// 5 ms or more between edges on the bench, so 1 ms sampling sees every state.
class EspGpioEncoder final : public hal::EncoderInterface {
public:
    explicit EspGpioEncoder(EspPinMap pins);
    ~EspGpioEncoder() override;

    void setListener(hal::EncoderListener* listener) noexcept override;
    core::Status start() override;
    void stop() noexcept override;
    [[nodiscard]] std::int32_t consumeDelta() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aw::platform::esp32p4
