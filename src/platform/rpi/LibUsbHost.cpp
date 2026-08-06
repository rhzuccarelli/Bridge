#include "aw/platform/rpi/LibUsbHost.hpp"

// Phase 8 replaces these stubs with libusb enumeration and hotplug.

namespace aw::platform::rpi {

struct LibUsbHost::Impl {
    hal::UsbHotplugListener* listener{nullptr};
    bool running{false};
};

LibUsbHost::LibUsbHost() : impl_{std::make_unique<Impl>()} {}
LibUsbHost::~LibUsbHost() = default;

core::Status LibUsbHost::start() { return core::notImplemented("LibUsbHost::start (Phase 8)"); }

void LibUsbHost::stop() noexcept { impl_->running = false; }

void LibUsbHost::setHotplugListener(hal::UsbHotplugListener* listener) noexcept {
    impl_->listener = listener;
}

core::Result<std::vector<hal::UsbDeviceInfo>> LibUsbHost::enumerate() const {
    return core::notImplemented("LibUsbHost::enumerate (Phase 8)");
}

}  // namespace aw::platform::rpi
