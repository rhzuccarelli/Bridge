#include "EspUsbStack.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

#include "aw/core/Logger.hpp"
#include "esp_intr_alloc.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_helpers.h"

namespace aw::platform::esp32p4::usb {
namespace {

constexpr std::string_view kModule = "usb";

constexpr std::uint8_t kClassAudio = 0x01;
constexpr std::uint8_t kSubclassMidiStreaming = 0x03;
constexpr std::uint8_t kClassStillImage = 0x06;
constexpr std::uint8_t kClassVendor = 0xFF;

std::string toAscii(const usb_str_desc_t* descriptor) {
    std::string text;
    if (descriptor == nullptr) return text;
    const int units = (descriptor->bLength - 2) / 2;
    for (int i = 0; i < units; ++i) {
        const std::uint16_t unit = descriptor->wData[i];
        text.push_back(unit < 0x80 ? static_cast<char>(unit) : '?');
    }
    return text;
}

/// The probe's classifier: an MTP interface is Still Image (or vendor, as some
/// devices declare it) with bulk IN, bulk OUT and an interrupt IN; MIDI OUT is
/// the bulk OUT endpoint of a MIDI Streaming interface.
Layout classify(const usb_config_desc_t* config) {
    Layout layout;
    if (config == nullptr) return layout;
    int offset = 0;
    const auto* descriptor = reinterpret_cast<const usb_standard_desc_t*>(config);
    const usb_intf_desc_t* current = nullptr;
    Endpoint in;
    Endpoint out;
    bool hasInterruptIn = false;

    auto finishInterface = [&] {
        if (current == nullptr) return;
        const bool mtpShaped = in.address != 0 && out.address != 0 && hasInterruptIn;
        if (!layout.hasMtp && mtpShaped &&
            (current->bInterfaceClass == kClassStillImage ||
             current->bInterfaceClass == kClassVendor)) {
            layout.hasMtp = true;
            layout.mtpInterface = current->bInterfaceNumber;
            layout.mtpAlternate = current->bAlternateSetting;
            layout.bulkIn = in;
            layout.bulkOut = out;
        }
        if (!layout.hasMidiOut && current->bInterfaceClass == kClassAudio &&
            current->bInterfaceSubClass == kSubclassMidiStreaming && out.address != 0) {
            layout.hasMidiOut = true;
            layout.midiInterface = current->bInterfaceNumber;
            layout.midiAlternate = current->bAlternateSetting;
            layout.midiOut = out;
        }
    };

    while ((descriptor = usb_parse_next_descriptor(descriptor, config->wTotalLength, &offset))) {
        if (descriptor->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            finishInterface();
            current = reinterpret_cast<const usb_intf_desc_t*>(descriptor);
            in = {};
            out = {};
            hasInterruptIn = false;
        } else if (descriptor->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT &&
                   current != nullptr) {
            const auto* ep = reinterpret_cast<const usb_ep_desc_t*>(descriptor);
            const int type = ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK;
            const bool isIn = (ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) != 0;
            const Endpoint endpoint{ep->bEndpointAddress,
                                    static_cast<std::uint16_t>(ep->wMaxPacketSize & 0x7FF)};
            if (type == USB_BM_ATTRIBUTES_XFER_BULK) (isIn ? in : out) = endpoint;
            if (type == USB_BM_ATTRIBUTES_XFER_INT && isIn) hasInterruptIn = true;
        }
    }
    finishInterface();
    return layout;
}

std::string hex4(std::uint16_t value) {
    char text[8];
    std::snprintf(text, sizeof(text), "%04x", value);
    return text;
}

}  // namespace

core::Status EspUsbStack::start() {
    std::lock_guard lock{mutex_};
    if (started_) return core::success();

    hotplug_ = xQueueCreate(8, sizeof(Hotplug));
    SemaphoreHandle_t installed = xSemaphoreCreateBinary();
    if (hotplug_ == nullptr || installed == nullptr) {
        return core::makeError(core::ErrorCode::OutOfMemory, "USB host queues");
    }
    if (xTaskCreatePinnedToCore(&EspUsbStack::hostTask, "usb_host", 4096, installed, 6, nullptr,
                                0) != pdPASS) {
        return core::makeError(core::ErrorCode::OutOfMemory, "USB host task");
    }
    xSemaphoreTake(installed, portMAX_DELAY);
    vSemaphoreDelete(installed);

    usb_host_client_config_t config{};
    config.is_synchronous = false;
    config.max_num_event_msg = 8;
    config.async.client_event_callback = &EspUsbStack::onClientEvent;
    config.async.callback_arg = this;
    if (const esp_err_t err = usb_host_client_register(&config, &client_); err != ESP_OK) {
        return core::makeError(core::ErrorCode::IoError,
                               std::string{"USB client: "} + esp_err_to_name(err));
    }
    if (xTaskCreatePinnedToCore(&EspUsbStack::clientTask, "usb_client", 4096, this, 5, nullptr,
                                0) != pdPASS ||
        xTaskCreate(&EspUsbStack::hotplugTask, "usb_hotplug", 6144, this, 4, nullptr) != pdPASS) {
        return core::makeError(core::ErrorCode::OutOfMemory, "USB client tasks");
    }
    started_ = true;
    core::log::info(kModule, "USB host on the high-speed port");
    return core::success();
}

void EspUsbStack::hostTask(void* installed) {
    usb_host_config_t config{};
    config.intr_flags = ESP_INTR_FLAG_LEVEL1;
    config.peripheral_map = BIT0;  // The high-speed OTG controller.
    ESP_ERROR_CHECK(usb_host_install(&config));
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(installed));
    for (;;) {
        std::uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
    }
}

void EspUsbStack::clientTask(void* context) {
    auto& self = *static_cast<EspUsbStack*>(context);
    for (;;) usb_host_client_handle_events(self.client_, portMAX_DELAY);
}

void EspUsbStack::onClientEvent(const usb_host_client_event_msg_t* message, void* context) {
    auto& self = *static_cast<EspUsbStack*>(context);
    Hotplug event;
    if (message->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        event.attached = true;
        event.address = message->new_dev.address;
    } else if (message->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        event.handle = message->dev_gone.dev_hdl;
    } else {
        return;
    }
    (void)xQueueSend(self.hotplug_, &event, 0);
}

void EspUsbStack::hotplugTask(void* context) {
    auto& self = *static_cast<EspUsbStack*>(context);
    for (;;) {
        Hotplug event;
        if (xQueueReceive(self.hotplug_, &event, portMAX_DELAY) != pdTRUE) continue;
        if (event.attached) {
            self.attach(event.address);
        } else {
            self.detach(event.handle);
        }
    }
}

void EspUsbStack::attach(std::uint8_t address) {
    usb_device_handle_t handle = nullptr;
    if (const esp_err_t err = usb_host_device_open(client_, address, &handle); err != ESP_OK) {
        core::log::warning(kModule, "cannot open USB device " + std::to_string(address) + ": " +
                                        esp_err_to_name(err));
        return;
    }
    usb_device_info_t details{};
    const usb_device_desc_t* device = nullptr;
    const usb_config_desc_t* config = nullptr;
    usb_host_device_info(handle, &details);
    usb_host_get_device_descriptor(handle, &device);
    usb_host_get_active_config_descriptor(handle, &config);

    Attached entry;
    entry.handle = handle;
    entry.layout = classify(config);
    entry.info.vendorId = device->idVendor;
    entry.info.productId = device->idProduct;
    entry.info.busNumber = 0;
    entry.info.deviceAddress = address;
    entry.info.manufacturer = toAscii(details.str_desc_manufacturer);
    entry.info.product = toAscii(details.str_desc_product);
    entry.info.serialNumber = toAscii(details.str_desc_serial_num);
    entry.info.supportsMtp = entry.layout.hasMtp;

    core::log::info(kModule, "attached " + hex4(entry.info.vendorId) + ":" +
                                 hex4(entry.info.productId) + " '" + entry.info.product + "'" +
                                 (details.speed == USB_SPEED_HIGH ? ", high speed" : ", NOT high speed") +
                                 (entry.layout.hasMtp ? ", MTP" : "") +
                                 (entry.layout.hasMidiOut ? ", MIDI OUT" : ""));

    hal::UsbHotplugListener* listener = nullptr;
    const hal::UsbDeviceInfo info = entry.info;
    {
        std::lock_guard lock{mutex_};
        devices_.push_back(std::move(entry));
        listener = listener_;
    }
    if (listener != nullptr) listener->onUsbDeviceAttached(info);
}

void EspUsbStack::detach(usb_device_handle_t handle) {
    hal::UsbHotplugListener* listener = nullptr;
    hal::UsbDeviceInfo info;
    {
        std::lock_guard lock{mutex_};
        auto device = std::find_if(devices_.begin(), devices_.end(),
                                   [&](const Attached& d) { return d.handle == handle; });
        if (device == devices_.end()) return;
        device->gone = true;
        info = device->info;
        listener = listener_;
        closeIfUnused(device);
    }
    core::log::info(kModule, "detached '" + info.product + "'");
    // A lease holder (the MTP session) sees isGone() from here on and skips
    // any I/O when the listener closes it.
    if (listener != nullptr) listener->onUsbDeviceDetached(info);
}

void EspUsbStack::closeIfUnused(std::vector<Attached>::iterator device) {
    if (!device->gone || device->leases > 0) return;
    usb_host_device_close(client_, device->handle);
    devices_.erase(device);
}

void EspUsbStack::setListener(hal::UsbHotplugListener* listener) noexcept {
    std::lock_guard lock{mutex_};
    listener_ = listener;
}

std::vector<hal::UsbDeviceInfo> EspUsbStack::devices() const {
    std::lock_guard lock{mutex_};
    std::vector<hal::UsbDeviceInfo> result;
    for (const Attached& device : devices_)
        if (!device.gone) result.push_back(device.info);
    return result;
}

std::optional<Lease> EspUsbStack::acquire(
    const std::function<bool(const hal::UsbDeviceInfo&, const Layout&)>& match) {
    std::lock_guard lock{mutex_};
    for (Attached& device : devices_) {
        if (device.gone || !match(device.info, device.layout)) continue;
        ++device.leases;
        return Lease{device.handle, device.layout, device.info};
    }
    return std::nullopt;
}

void EspUsbStack::release(const Lease& lease) noexcept {
    std::lock_guard lock{mutex_};
    auto device = std::find_if(devices_.begin(), devices_.end(),
                               [&](const Attached& d) { return d.handle == lease.handle; });
    if (device == devices_.end()) return;
    --device->leases;
    closeIfUnused(device);
}

bool EspUsbStack::isGone(const Lease& lease) const {
    std::lock_guard lock{mutex_};
    const auto device = std::find_if(devices_.begin(), devices_.end(),
                                     [&](const Attached& d) { return d.handle == lease.handle; });
    return device == devices_.end() || device->gone;
}

}  // namespace aw::platform::esp32p4::usb
