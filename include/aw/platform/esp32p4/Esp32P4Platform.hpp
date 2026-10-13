#pragma once

#include <cstdint>

#include "aw/core/Result.hpp"
#include "aw/core/Types.hpp"
#include "aw/hal/Platform.hpp"
#include "aw/platform/esp32p4/EspGpioInput.hpp"

namespace aw::platform::esp32p4 {

struct Esp32P4Config {
    /// Where projects will live once the SD card is mounted (step P2).
    core::Path projectsRoot{"/sdcard/projects"};

    /// Minimum time between two display frames printed to the serial log.
    /// One frame is about 12 KiB, roughly a second of UART time at 115200 baud.
    std::uint32_t previewIntervalMillis{2000};

    /// Encoder and button wiring.
    EspPinMap pins{EspPinMap::picoPrototype()};
};

/// Builds the ESP32-P4 `hal::Platform`.
///
/// The clock and the encoder with its push switch (`EspGpioEncoder`,
/// `EspGpioButtons`) are real hardware. The other backends are stand-ins that
/// let the whole portable application boot and run unchanged:
///  - storage is an empty volume that refuses writes (FatFS on SD is P2),
///  - the display is the real SSD1306 logic over a bus that goes nowhere, with
///    each changed frame printed to the serial log (the OLED is P2),
///  - audio output is `NullAudioOutput`, which still drives the engine on a
///    wall clock (I2S is P3),
///  - USB reports no devices and MTP is absent (P4).
[[nodiscard]] core::Result<hal::Platform> createPlatform(const Esp32P4Config& config);

}  // namespace aw::platform::esp32p4
