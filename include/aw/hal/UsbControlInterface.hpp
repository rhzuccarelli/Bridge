#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "aw/core/Result.hpp"

namespace aw::hal {

/// The setup packet of a USB control request, apart from wLength (the size of
/// the data span).
struct UsbControlSetup {
    std::uint8_t requestType{0};  ///< bmRequestType; bit 7 set reads from the device.
    std::uint8_t request{0};
    std::uint16_t value{0};
    std::uint16_t index{0};
};

/// Control requests on endpoint 0 of an attached device, for devices whose
/// settings are vendor requests (an Apogee Duet's input types).
///
/// Narrow on purpose, like `USBInterface`: no interface claims, no bulk or
/// isochronous transfers. Endpoint 0 belongs to no interface, so these work
/// while a class driver streams the device's audio.
class UsbControlInterface {
public:
    virtual ~UsbControlInterface() = default;

    UsbControlInterface(const UsbControlInterface&) = delete;
    UsbControlInterface& operator=(const UsbControlInterface&) = delete;

    /// Sends `setup` to the first attached device with this vendor and product
    /// ID. A read fills `data` and returns the bytes received; a write sends
    /// `data` and returns its size. `DeviceNotConnected` when no such device
    /// is attached. Blocking: call it from a worker, not the UI loop.
    [[nodiscard]] virtual core::Result<std::size_t> control(std::uint16_t vendorId,
                                                            std::uint16_t productId,
                                                            const UsbControlSetup& setup,
                                                            std::span<std::uint8_t> data) = 0;

protected:
    UsbControlInterface() = default;
};

}  // namespace aw::hal
