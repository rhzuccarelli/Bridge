#pragma once

#include <array>
#include <memory>
#include <string>

#include "aw/hal/ButtonInterface.hpp"
#include "aw/hal/EncoderInterface.hpp"

namespace aw::platform::rpi {

/// Which GPIO line each control sits on.
///
/// A table, not constants scattered through the code, so wiring the prototype
/// differently is a single edit and so the same code can serve a revised board.
struct GpioPinMap {
    std::array<int, core::kButtonCount> buttonLines{};
    int encoderLineA{0};
    int encoderLineB{0};

    /// A GPIO driven permanently high, used as the encoder module's 3.3 V supply.
    /// Negative means "not used - the module is wired to a real 3.3 V pin".
    ///
    /// This exists because the prototype's LCD HAT occupies header pins 1-26,
    /// which is where *both* 3.3 V pins live.  A KY-040 cannot simply run with
    /// its `+` floating: its three 10k pull-ups share that node, so closing one
    /// contact drags the other two lines to roughly 1.2 V - right in the
    /// indeterminate band between logic low and logic high, which reads as an
    /// intermittently broken encoder rather than as a wiring fault.
    ///
    /// Driving a spare GPIO high instead is safe: the module draws at most
    /// ~1 mA (3 x 3.3 V / 10k with every contact closed), far below the 16 mA a
    /// Pi pin can source.  The line must be requested as an output *before* the
    /// encoder lines are read, and the request held open for as long as the
    /// encoder is in use.
    int encoderPowerLine{-1};

    /// Which gpiochip carries the 40-pin header.
    ///
    /// Not a constant, because it moved: on a Pi 5 the header lives behind the
    /// RP1 southbridge, which older Bookworm kernels expose as `gpiochip4` and
    /// newer ones as `gpiochip0`.  Run `gpiodetect` and use whichever reports
    /// 54 lines; `AW_GPIO_CHIP` overrides it at runtime.
    std::string chipName{"gpiochip0"};

    /// The prototype's wiring.  Documented in docs/hardware.md.
    [[nodiscard]] static GpioPinMap prototype() noexcept;
};

/// Buttons over libgpiod.  (Phase 6.)
///
/// Uses gpiod edge events on a dedicated poll thread rather than polling levels:
/// a missed press on a hardware recorder is unacceptable, and edge events plus a
/// short debounce window catch every transition without burning CPU.
/// Debounce, long-press (500 ms) and auto-repeat live here, so the event stream
/// leaving this class is already clean.
class GpioButtons final : public hal::ButtonInterface {
public:
    explicit GpioButtons(GpioPinMap pins);
    ~GpioButtons() override;

    void setListener(hal::ButtonListener* listener) noexcept override;
    core::Status start() override;
    void stop() noexcept override;
    [[nodiscard]] bool isPressed(core::ButtonId id) const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Rotary encoder over libgpiod.  (Phase 6.)
///
/// Quadrature decoding with a state table, coalescing the four quadrature
/// transitions per detent into one reported step, so one physical click always
/// moves the UI by exactly one row.
class GpioEncoder final : public hal::EncoderInterface {
public:
    explicit GpioEncoder(GpioPinMap pins);
    ~GpioEncoder() override;

    void setListener(hal::EncoderListener* listener) noexcept override;
    core::Status start() override;
    void stop() noexcept override;
    [[nodiscard]] std::int32_t consumeDelta() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aw::platform::rpi
