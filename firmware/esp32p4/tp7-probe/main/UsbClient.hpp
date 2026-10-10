#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"

namespace probe {

struct UsbEvent {
    enum class Kind : std::uint8_t { Attached, Detached };
    Kind kind{Kind::Attached};
    std::uint8_t address{0};                  ///< Attached only.
    usb_device_handle_t device{nullptr};      ///< Detached only.
};

/// One USB Host Library client.
///
/// The library delivers both client events and transfer completions from
/// `usb_host_client_handle_events`, so that call gets a task of its own and
/// never does anything else. Blocking work happens on the caller's task and
/// waits for completions; doing it inside the event callback would deadlock.
class UsbClient {
public:
    esp_err_t start();
    bool waitEvent(UsbEvent& event, TickType_t timeout);
    [[nodiscard]] usb_host_client_handle_t handle() const { return client_; }

private:
    static void onEvent(const usb_host_client_event_msg_t* message, void* context);
    static void pump(void* context);

    usb_host_client_handle_t client_{nullptr};
    QueueHandle_t events_{nullptr};
};

/// A blocking pipe over one bulk endpoint.
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

    /// Receives up to `wanted` bytes (capped at the buffer); a short packet
    /// ends the transfer early. The bytes are in `data()`.
    esp_err_t read(std::size_t wanted, std::size_t& received, std::uint32_t timeoutMs);

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

}  // namespace probe
