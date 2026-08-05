#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "aw/core/Result.hpp"

namespace aw::hal {

/// Identity of an attached USB device, as far as the host can see it before
/// opening a protocol session.
struct UsbDeviceInfo {
    std::uint16_t vendorId{0};
    std::uint16_t productId{0};
    std::uint8_t busNumber{0};
    std::uint8_t deviceAddress{0};
    std::string manufacturer;
    std::string product;
    std::string serialNumber;

    /// True when the device advertises the MTP/PTP still-image class or the
    /// MTP interface string.  The MTP layer only ever looks at these.
    bool supportsMtp{false};

    /// Stable within one attachment.  Handed to `MTPInterface::openSession`.
    [[nodiscard]] std::uint32_t key() const noexcept {
        return (static_cast<std::uint32_t>(busNumber) << 8) | deviceAddress;
    }
};

class UsbHotplugListener {
public:
    virtual ~UsbHotplugListener() = default;

    virtual void onUsbDeviceAttached(const UsbDeviceInfo& device) = 0;
    virtual void onUsbDeviceDetached(const UsbDeviceInfo& device) = 0;
};

/// USB host controller.
///
/// Scope is deliberately narrow: enumerate devices and report hotplug.  It does
/// not do bulk transfers, because on the Pi those happen inside libmtp and on
/// the ESP32-P4 inside a TinyUSB MTP initiator.  Exposing raw transfers here
/// would force one of the two backends to fake an API it does not have.
class USBInterface {
public:
    virtual ~USBInterface() = default;

    USBInterface(const USBInterface&) = delete;
    USBInterface& operator=(const USBInterface&) = delete;

    virtual core::Status start() = 0;
    virtual void stop() noexcept = 0;

    virtual void setHotplugListener(UsbHotplugListener* listener) noexcept = 0;

    [[nodiscard]] virtual core::Result<std::vector<UsbDeviceInfo>> enumerate() const = 0;

protected:
    USBInterface() = default;
};

}  // namespace aw::hal
