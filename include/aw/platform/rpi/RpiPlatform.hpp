#pragma once

#include <string>

#include "aw/core/Result.hpp"
#include "aw/core/Types.hpp"
#include "aw/hal/Platform.hpp"
#include "aw/platform/rpi/GpioInput.hpp"
#include "aw/platform/rpi/SpiDisplay.hpp"

namespace aw::platform::rpi {

/// Everything the Pi build needs to know about its own hardware.
struct RpiConfig {
    core::Path projectsRoot{"/var/lib/audio-workstation/projects"};
    std::string alsaDevice{"hw:TP7,0"};
    std::string midiDevice{"hw:TP7,0,0"};
    GpioPinMap pins{GpioPinMap::prototype()};
    SpiDisplayConfig display{};
};

/// Builds the Raspberry Pi 5 `hal::Platform`.
///
/// This function is the *only* place in the application where ALSA, libgpiod,
/// spidev, libusb and libmtp are named.  Everything else talks to interfaces.
/// The ESP32-P4 port adds a sibling `createPlatform` and changes nothing else.
[[nodiscard]] core::Result<hal::Platform> createPlatform(const RpiConfig& config);

}  // namespace aw::platform::rpi
