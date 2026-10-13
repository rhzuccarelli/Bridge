#include "BulkPipe.hpp"

#include <algorithm>
#include <cstring>

#include "usb/usb_helpers.h"

namespace aw::platform::esp32p4::usb {

BulkPipe::BulkPipe(usb_device_handle_t device, std::uint8_t endpoint, std::uint16_t maxPacket)
    : device_{device}, endpoint_{endpoint}, maxPacket_{maxPacket} {}

BulkPipe::~BulkPipe() {
    if (transfer_ != nullptr) usb_host_transfer_free(transfer_);
    if (done_ != nullptr) vSemaphoreDelete(done_);
}

esp_err_t BulkPipe::init(std::size_t bufferBytes) {
    if (maxPacket_ == 0) return ESP_ERR_INVALID_ARG;
    done_ = xSemaphoreCreateBinary();
    if (done_ == nullptr) return ESP_ERR_NO_MEM;
    const std::size_t rounded = usb_round_up_to_mps(static_cast<int>(bufferBytes), maxPacket_);
    if (const esp_err_t err = usb_host_transfer_alloc(rounded, 0, &transfer_); err != ESP_OK)
        return err;
    transfer_->device_handle = device_;
    transfer_->bEndpointAddress = endpoint_;
    transfer_->callback = &BulkPipe::completed;
    transfer_->context = this;
    return ESP_OK;
}

std::uint8_t* BulkPipe::buffer() { return transfer_->data_buffer; }
const std::uint8_t* BulkPipe::data() const { return transfer_->data_buffer; }
std::size_t BulkPipe::capacity() const { return transfer_->data_buffer_size; }

void BulkPipe::completed(usb_transfer_t* transfer) {
    auto& self = *static_cast<BulkPipe*>(transfer->context);
    xSemaphoreGive(self.done_);
}

esp_err_t BulkPipe::run(std::size_t bytes, std::uint32_t flags, std::uint32_t timeoutMs) {
    transfer_->num_bytes = static_cast<int>(bytes);
    transfer_->flags = flags;
    if (const esp_err_t err = usb_host_transfer_submit(transfer_); err != ESP_OK) return err;

    if (xSemaphoreTake(done_, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
        // The library has no per-transfer timeout: halting and flushing the
        // endpoint retires the transfer with CANCELED.
        usb_host_endpoint_halt(device_, endpoint_);
        usb_host_endpoint_flush(device_, endpoint_);
        xSemaphoreTake(done_, portMAX_DELAY);
        usb_host_endpoint_clear(device_, endpoint_);
        lastStatus_ = USB_TRANSFER_STATUS_TIMED_OUT;
        return ESP_ERR_TIMEOUT;
    }
    lastStatus_ = transfer_->status;
    if (lastStatus_ == USB_TRANSFER_STATUS_COMPLETED) return ESP_OK;
    if (lastStatus_ == USB_TRANSFER_STATUS_NO_DEVICE) return ESP_ERR_INVALID_STATE;
    return ESP_FAIL;
}

esp_err_t BulkPipe::send(std::size_t bytes, bool terminate, std::uint32_t timeoutMs) {
    return run(bytes, terminate ? USB_TRANSFER_FLAG_ZERO_PACK : 0U, timeoutMs);
}

esp_err_t BulkPipe::write(std::span<const std::uint8_t> data, bool terminate,
                          std::uint32_t timeoutMs) {
    do {
        const std::size_t chunk = std::min(data.size(), capacity());
        std::memcpy(transfer_->data_buffer, data.data(), chunk);
        const bool last = chunk == data.size();
        if (const esp_err_t err = send(chunk, last && terminate, timeoutMs); err != ESP_OK)
            return err;
        data = data.subspan(chunk);
    } while (!data.empty());
    return ESP_OK;
}

esp_err_t BulkPipe::read(std::size_t wanted, std::size_t& received, std::uint32_t timeoutMs) {
    // IN transfers must request whole packets; the device ends early with a
    // short packet when it has less.
    const std::size_t rounded = std::min(
        capacity(), static_cast<std::size_t>(usb_round_up_to_mps(
                        static_cast<int>(std::max<std::size_t>(wanted, 1)), maxPacket_)));
    received = 0;
    const esp_err_t err = run(rounded, 0U, timeoutMs);
    if (err == ESP_OK) received = static_cast<std::size_t>(transfer_->actual_num_bytes);
    return err;
}

const char* transferStatusName(usb_transfer_status_t status) {
    switch (status) {
        case USB_TRANSFER_STATUS_COMPLETED: return "completed";
        case USB_TRANSFER_STATUS_ERROR: return "error";
        case USB_TRANSFER_STATUS_TIMED_OUT: return "timed out";
        case USB_TRANSFER_STATUS_CANCELED: return "canceled";
        case USB_TRANSFER_STATUS_STALL: return "stall";
        case USB_TRANSFER_STATUS_OVERFLOW: return "overflow";
        case USB_TRANSFER_STATUS_SKIPPED: return "skipped";
        case USB_TRANSFER_STATUS_NO_DEVICE: return "no device";
    }
    return "unknown";
}

}  // namespace aw::platform::esp32p4::usb
