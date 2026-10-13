#pragma once

#include <memory>

#include "aw/hal/DeviceModeInterface.hpp"
#include "aw/hal/MTPInterface.hpp"
#include "aw/hal/USBInterface.hpp"

namespace aw::platform::esp32p4 {

/// The three USB-facing backends, sharing one ESP-IDF USB host on the P4's
/// high-speed port (the TP-7 header on the P4-Pico):
///  - `usb`: hotplug and enumeration,
///  - `mtp`: a PTP/MTP initiator over the device's bulk pipes (grown from the
///    TP-7 probe; reads with GetPartialObject, writes with SendObjectInfo +
///    SendObject),
///  - `deviceMode`: switches a TP-7 in USB audio mode to MTP by sending the
///    Field Kit SysEx as USB-MIDI packets, as `AlsaTp7ModeSwitcher` does
///    through ALSA on the Pi.
struct EspUsbBackends {
    std::unique_ptr<hal::USBInterface> usb;
    std::unique_ptr<hal::MTPInterface> mtp;
    std::unique_ptr<hal::DeviceModeInterface> deviceMode;
};

[[nodiscard]] EspUsbBackends createUsbBackends();

}  // namespace aw::platform::esp32p4
