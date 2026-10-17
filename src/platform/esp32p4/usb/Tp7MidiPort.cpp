#include "Tp7MidiPort.hpp"

#include <string>

#include "BulkPipe.hpp"
#include "aw/core/Logger.hpp"

namespace aw::platform::esp32p4::usb {
namespace {

constexpr std::string_view kModule = "tp7-midi";
constexpr std::uint16_t kTeenageEngineering = 0x2367;
constexpr std::size_t kInBytes = 512;

}  // namespace

Tp7MidiPort::Tp7MidiPort(std::shared_ptr<EspUsbStack> stack)
    : stack_{std::move(stack)}, inStopped_{xSemaphoreCreateBinary()} {
    stack_->addInternalListener(this);
}

Tp7MidiPort::~Tp7MidiPort() {
    std::lock_guard lock{mutex_};
    closeLocked();
    vSemaphoreDelete(inStopped_);
}

void Tp7MidiPort::onUsbDeviceAttached(const hal::UsbDeviceInfo& device) {
    if (device.vendorId != kTeenageEngineering || device.supportsMtp) return;
    std::lock_guard lock{mutex_};
    if (lease_) return;
    const std::uint32_t key = device.key();
    lease_ = stack_->acquire([key](const hal::UsbDeviceInfo& info, const Layout& layout) {
        return info.key() == key && layout.hasMidiOut;
    });
    if (!lease_) return;
    const Layout& layout = lease_->layout;
    if (usb_host_interface_claim(stack_->client(), lease_->handle, layout.midiInterface,
                                 layout.midiAlternate) != ESP_OK) {
        core::log::warning(kModule, "cannot claim the TP-7's MIDI interface");
        stack_->release(*lease_);
        lease_.reset();
        return;
    }
    if (layout.midiIn.address == 0 || usb_host_transfer_alloc(kInBytes, 0, &in_) != ESP_OK) return;
    in_->device_handle = lease_->handle;
    in_->bEndpointAddress = layout.midiIn.address;
    in_->num_bytes = static_cast<int>(kInBytes);
    in_->callback = &Tp7MidiPort::inDone;
    in_->context = this;
    (void)xSemaphoreTake(inStopped_, 0);  // A stop left over from the last attach.
    reading_.store(true);
    if (usb_host_transfer_submit(in_) != ESP_OK) {
        reading_.store(false);
        usb_host_transfer_free(in_);
        in_ = nullptr;
    }
}

void Tp7MidiPort::onUsbDeviceDetached(const hal::UsbDeviceInfo& device) {
    std::lock_guard lock{mutex_};
    if (lease_ && lease_->info.key() == device.key()) closeLocked();
}

void Tp7MidiPort::closeLocked() noexcept {
    if (!lease_) return;
    if (in_ != nullptr) {
        if (reading_.exchange(false)) {
            // A pending bulk read never completes on its own: halt and flush
            // the endpoint, which retires it.
            (void)usb_host_endpoint_halt(lease_->handle, lease_->layout.midiIn.address);
            (void)usb_host_endpoint_flush(lease_->handle, lease_->layout.midiIn.address);
            (void)usb_host_endpoint_clear(lease_->handle, lease_->layout.midiIn.address);
            (void)xSemaphoreTake(inStopped_, pdMS_TO_TICKS(500));
        }
        usb_host_transfer_free(in_);
        in_ = nullptr;
    }
    (void)usb_host_interface_release(stack_->client(), lease_->handle, lease_->layout.midiInterface);
    stack_->release(*lease_);
    lease_.reset();
}

void Tp7MidiPort::inDone(usb_transfer_t* transfer) {
    auto& self = *static_cast<Tp7MidiPort*>(transfer->context);
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED)
        self.received_.fetch_add(static_cast<std::uint32_t>(transfer->actual_num_bytes / 4));
    if (self.reading_.load() && transfer->status == USB_TRANSFER_STATUS_COMPLETED &&
        usb_host_transfer_submit(transfer) == ESP_OK)
        return;
    self.reading_.store(false);
    xSemaphoreGive(self.inStopped_);
}

core::Status Tp7MidiPort::send(std::span<const std::uint8_t> packets) {
    std::lock_guard lock{mutex_};
    if (!lease_ || stack_->isGone(*lease_)) {
        return core::makeError(core::ErrorCode::DeviceNotConnected, "no TP-7 in USB-audio mode");
    }
    const Layout& layout = lease_->layout;
    BulkPipe out{lease_->handle, layout.midiOut.address, layout.midiOut.maxPacket};
    if (out.init(64) != ESP_OK || out.write(packets, false, 1000) != ESP_OK) {
        return core::makeError(core::ErrorCode::IoError, std::string{"MIDI to the TP-7 failed: "} +
                                                             transferStatusName(out.lastStatus()));
    }
    return core::success();
}

}  // namespace aw::platform::esp32p4::usb
