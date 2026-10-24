#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "aw/hal/ButtonInterface.hpp"
#include "aw/hal/EncoderInterface.hpp"

namespace aw::platform::esp32p4 {

/// Development input: keys typed on the serial console act as the panel's
/// buttons and encoder, so a UI flow can be driven from the Mac (by hand or
/// by a script) without touching the hardware. Merged with the GPIO controls
/// through `CompositeButtons` / `CompositeEncoder`; idle when nothing is typed.
///
/// The layout follows the desktop build's keyboard:
///
///   space  Play      i  Import     1..6     Track 1..6
///   s      Stop      e  Export     Enter    Encoder push
///   r      Record    u  Undo       j / k    Encoder one detent cw / ccw
///   w      Save      b  Back       arrows   Encoder (right/down cw, left/up ccw)
class EspConsoleInput {
public:
    EspConsoleInput();
    ~EspConsoleInput();
    EspConsoleInput(const EspConsoleInput&) = delete;
    EspConsoleInput& operator=(const EspConsoleInput&) = delete;

    /// The two HAL faces of the one console reader.
    [[nodiscard]] std::unique_ptr<hal::ButtonInterface> buttons();
    [[nodiscard]] std::unique_ptr<hal::EncoderInterface> encoder();

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

}  // namespace aw::platform::esp32p4
