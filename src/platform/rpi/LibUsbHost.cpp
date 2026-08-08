#include "aw/platform/rpi/LibUsbHost.hpp"

#include <atomic>
#include <chrono>
#include <libusb.h>
#include <thread>

namespace aw::platform::rpi {
namespace {

core::Error usbError(int code, std::string_view operation) {
    core::ErrorCode mapped = code == LIBUSB_ERROR_ACCESS ? core::ErrorCode::PermissionDenied
                           : code == LIBUSB_ERROR_BUSY ? core::ErrorCode::DeviceBusy
                           : code == LIBUSB_ERROR_TIMEOUT ? core::ErrorCode::Timeout
                                                        : core::ErrorCode::IoError;
    return core::makeError(mapped, std::string{operation} + ": " + libusb_error_name(code));
}

std::string readString(libusb_device_handle* handle, std::uint8_t index) {
    if (handle == nullptr || index == 0) return {};
    unsigned char text[256]{};
    const int size = libusb_get_string_descriptor_ascii(handle, index, text, sizeof(text));
    return size > 0 ? std::string{reinterpret_cast<char*>(text), static_cast<std::size_t>(size)}
                    : std::string{};
}

core::Result<hal::UsbDeviceInfo> describe(libusb_device* device) {
    libusb_device_descriptor descriptor{};
    const int result = libusb_get_device_descriptor(device, &descriptor);
    if (result != LIBUSB_SUCCESS) return usbError(result, "USB descriptor");

    bool supportsMtp = false;
    for (std::uint8_t index = 0; index < descriptor.bNumConfigurations; ++index) {
        libusb_config_descriptor* config = nullptr;
        if (libusb_get_config_descriptor(device, index, &config) != LIBUSB_SUCCESS) continue;
        for (std::uint8_t interfaceIndex = 0; interfaceIndex < config->bNumInterfaces; ++interfaceIndex) {
            const libusb_interface& interface = config->interface[interfaceIndex];
            for (int alt = 0; alt < interface.num_altsetting; ++alt) {
                supportsMtp = supportsMtp ||
                              interface.altsetting[alt].bInterfaceClass == LIBUSB_CLASS_IMAGE;
            }
        }
        libusb_free_config_descriptor(config);
    }
    // Confirmed TP-7 identity. This also covers firmware descriptors that do
    // not expose the still-image class until the device enters MTP mode.
    supportsMtp = supportsMtp || (descriptor.idVendor == 0x2367 && descriptor.idProduct == 0x0019);

    hal::UsbDeviceInfo info;
    info.vendorId = descriptor.idVendor;
    info.productId = descriptor.idProduct;
    info.busNumber = libusb_get_bus_number(device);
    info.deviceAddress = libusb_get_device_address(device);
    info.supportsMtp = supportsMtp;

    libusb_device_handle* handle = nullptr;
    if (libusb_open(device, &handle) == LIBUSB_SUCCESS) {
        info.manufacturer = readString(handle, descriptor.iManufacturer);
        info.product = readString(handle, descriptor.iProduct);
        info.serialNumber = readString(handle, descriptor.iSerialNumber);
        libusb_close(handle);
    }
    return info;
}

}  // namespace

struct LibUsbHost::Impl {
    libusb_context* context{nullptr};
    libusb_hotplug_callback_handle callback{};
    std::atomic<hal::UsbHotplugListener*> listener{nullptr};
    std::atomic<bool> running{false};
    std::thread eventThread;
};

LibUsbHost::LibUsbHost() : impl_{std::make_unique<Impl>()} {}
LibUsbHost::~LibUsbHost() { stop(); }

core::Status LibUsbHost::start() {
    if (impl_->running.load(std::memory_order_acquire)) return core::success();
    const int init = libusb_init(&impl_->context);
    if (init != LIBUSB_SUCCESS) return usbError(init, "libusb init");
    const auto callback = [](libusb_context*, libusb_device* device,
                             libusb_hotplug_event event, void* data) -> int {
        auto& impl = *static_cast<Impl*>(data);
        hal::UsbHotplugListener* listener = impl.listener.load(std::memory_order_acquire);
        if (listener == nullptr) return 0;
        auto info = describe(device);
        if (!info || !info.value().supportsMtp) return 0;
        if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED)
            listener->onUsbDeviceAttached(info.value());
        if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT)
            listener->onUsbDeviceDetached(info.value());
        return 0;
    };
    const int registered = libusb_hotplug_register_callback(
        impl_->context,
        static_cast<libusb_hotplug_event>(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
                                         LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
        LIBUSB_HOTPLUG_NO_FLAGS, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY,
        LIBUSB_HOTPLUG_MATCH_ANY, callback, impl_.get(), &impl_->callback);
    if (registered != LIBUSB_SUCCESS) {
        libusb_exit(impl_->context);
        impl_->context = nullptr;
        return usbError(registered, "USB hotplug registration");
    }
    impl_->running.store(true, std::memory_order_release);
    impl_->eventThread = std::thread([this] {
        while (impl_->running.load(std::memory_order_acquire)) {
            timeval timeout{0, 250000};
            (void)libusb_handle_events_timeout_completed(impl_->context, &timeout, nullptr);
        }
    });
    return core::success();
}

void LibUsbHost::stop() noexcept {
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) return;
    if (impl_->eventThread.joinable()) impl_->eventThread.join();
    libusb_hotplug_deregister_callback(impl_->context, impl_->callback);
    libusb_exit(impl_->context);
    impl_->context = nullptr;
}

void LibUsbHost::setHotplugListener(hal::UsbHotplugListener* listener) noexcept {
    impl_->listener.store(listener, std::memory_order_release);
}

core::Result<std::vector<hal::UsbDeviceInfo>> LibUsbHost::enumerate() const {
    if (impl_->context == nullptr) return core::makeError(core::ErrorCode::InvalidState, "USB not started");
    libusb_device** devices = nullptr;
    const ssize_t count = libusb_get_device_list(impl_->context, &devices);
    if (count < 0) return usbError(static_cast<int>(count), "USB enumeration");
    std::vector<hal::UsbDeviceInfo> result;
    for (ssize_t index = 0; index < count; ++index) {
        auto info = describe(devices[index]);
        if (info) result.push_back(std::move(info).value());
    }
    libusb_free_device_list(devices, 1);
    return result;
}

}  // namespace aw::platform::rpi
