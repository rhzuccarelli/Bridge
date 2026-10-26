#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include "aw/core/Result.hpp"
#include "aw/hal/AudioOutputInterface.hpp"
#include "aw/hal/DeviceModeInterface.hpp"
#include "aw/hal/MTPInterface.hpp"
#include "aw/hal/USBInterface.hpp"
#include "aw/hal/UsbControlInterface.hpp"

namespace aw::platform::esp32p4 {

/// The USB-facing backends, sharing one ESP-IDF USB host on the P4's
/// high-speed port (the TP-7 header on the P4-Pico):
///  - `usb`: hotplug and enumeration,
///  - `mtp`: a PTP/MTP initiator over the device's bulk pipes (grown from the
///    TP-7 probe; reads with GetPartialObject, writes with SendObjectInfo +
///    SendObject),
///  - `deviceMode`: switches a TP-7 in USB audio mode to MTP by sending the
///    Field Kit SysEx as USB-MIDI packets, as `AlsaTp7ModeSwitcher` does
///    through ALSA on the Pi,
///  - `audioOutput`: monitor audio through the TP-7's USB playback endpoint
///    (48 kHz stereo from the engine, sent as 96 kHz six-channel); fails to
///    open while no TP-7 is in USB-audio mode,
///  - `usbControl`: vendor requests on endpoint 0 of any attached device (the
///    Apogee Duet's input settings),
///  - `sendMidi`: raw USB-MIDI event packets (4 bytes each, cable 0) to the
///    TP-7, for bench tooling such as arming a recording.
struct EspUsbBackends {
    std::unique_ptr<hal::USBInterface> usb;
    std::unique_ptr<hal::MTPInterface> mtp;
    std::unique_ptr<hal::DeviceModeInterface> deviceMode;
    std::unique_ptr<hal::AudioOutputInterface> audioOutput;
    std::unique_ptr<hal::UsbControlInterface> usbControl;
    std::function<core::Status(std::span<const std::uint8_t>)> sendMidi;
};

[[nodiscard]] EspUsbBackends createUsbBackends();

}  // namespace aw::platform::esp32p4
