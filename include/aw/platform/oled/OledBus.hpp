#pragma once

#include <cstdint>
#include <span>

#include "aw/core/Result.hpp"

namespace aw::platform::oled {

/// The wire to a command/data OLED controller.
///
/// This is the seam that keeps the SSD1306/SH1106 protocol testable. The
/// controller logic (init sequence, framebuffer packing, flush) speaks only to
/// this interface, so a fake bus can record every byte and a test can assert on
/// the exact command stream with no SPI, no GPIO and no hardware. The real
/// implementation (`linux::SpiOledBus`) does spidev + GPIO; a test double does
/// nothing but remember.
///
/// The command/data distinction is physical: on these panels a dedicated D/C
/// GPIO tells the controller whether the bytes on the SPI bus are commands or
/// pixel data, so it is part of the bus contract rather than something encoded
/// in the byte stream.
class OledBus {
public:
    virtual ~OledBus() = default;

    OledBus(const OledBus&) = delete;
    OledBus& operator=(const OledBus&) = delete;

    /// Pulses the reset line: low, brief hold, high. Called once before init.
    virtual core::Status reset() = 0;

    /// Sends bytes with D/C low (commands).
    virtual core::Status writeCommands(std::span<const std::uint8_t> commands) = 0;

    /// Sends bytes with D/C high (display RAM).
    virtual core::Status writeData(std::span<const std::uint8_t> data) = 0;

protected:
    OledBus() = default;
};

}  // namespace aw::platform::oled
