#pragma once

#include <memory>

#include "aw/hal/USBInterface.hpp"

namespace aw::platform::rpi {

/// USB host enumeration and hotplug via libusb.  (Phase 8.)
///
/// Runs libusb's event loop on its own thread and reports attach/detach through
/// the listener.  MTP capability is decided here by looking at the interface
/// class (still image / PTP) and the MTP interface string descriptor, so the
/// portable `MtpManager` never has to know a USB descriptor exists.
class LibUsbHost final : public hal::USBInterface {
public:
    LibUsbHost();
    ~LibUsbHost() override;

    core::Status start() override;
    void stop() noexcept override;
    void setHotplugListener(hal::UsbHotplugListener* listener) noexcept override;
    [[nodiscard]] core::Result<std::vector<hal::UsbDeviceInfo>> enumerate() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aw::platform::rpi
