#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"

namespace aw::platform::esp32p4::usb {

/// A blocking pipe over one bulk endpoint, as proven by the TP-7 probe.
///
/// Completions arrive on the USB client task (`EspUsbStack`), so these calls
/// must never be made from that task.
class BulkPipe {
public:
    BulkPipe(usb_device_handle_t device, std::uint8_t endpoint, std::uint16_t maxPacket);
    ~BulkPipe();

    BulkPipe(const BulkPipe&) = delete;
    BulkPipe& operator=(const BulkPipe&) = delete;

    /// Allocates the transfer buffer, rounded up to a whole number of packets.
    esp_err_t init(std::size_t bufferBytes);

    /// Sends `data`, splitting it across transfers when it is larger than the
    /// buffer. With `terminate`, a phase that ends on a packet boundary is
    /// closed with a zero-length packet, as the Still Image class requires.
    esp_err_t write(std::span<const std::uint8_t> data, bool terminate, std::uint32_t timeoutMs);

    /// Sends the first `bytes` of `buffer()`, for callers that fill it in place.
    esp_err_t send(std::size_t bytes, bool terminate, std::uint32_t timeoutMs);

    /// Receives up to `wanted` bytes (capped at the buffer); a short packet
    /// ends the transfer early. The bytes are in `data()`.
    esp_err_t read(std::size_t wanted, std::size_t& received, std::uint32_t timeoutMs);

    [[nodiscard]] std::uint8_t* buffer();
    [[nodiscard]] const std::uint8_t* data() const;
    [[nodiscard]] std::size_t capacity() const;
    [[nodiscard]] std::uint16_t maxPacket() const { return maxPacket_; }
    [[nodiscard]] usb_transfer_status_t lastStatus() const { return lastStatus_; }

private:
    esp_err_t run(std::size_t bytes, std::uint32_t flags, std::uint32_t timeoutMs);
    static void completed(usb_transfer_t* transfer);

    usb_device_handle_t device_;
    std::uint8_t endpoint_;
    std::uint16_t maxPacket_;
    usb_transfer_t* transfer_{nullptr};
    SemaphoreHandle_t done_{nullptr};
    usb_transfer_status_t lastStatus_{USB_TRANSFER_STATUS_COMPLETED};
};

[[nodiscard]] const char* transferStatusName(usb_transfer_status_t status);

/// One control transfer on EP0, waited for (up to 1 s). `data` is sent for an
/// OUT request and filled for an IN request; it may be null when wLength is 0.
/// Never call this on the USB client task.
esp_err_t controlTransfer(usb_host_client_handle_t client, usb_device_handle_t device,
                          const usb_setup_packet_t& setup, std::uint8_t* data);

}  // namespace aw::platform::esp32p4::usb
