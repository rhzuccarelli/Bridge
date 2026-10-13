#include "UsbClient.hpp"

#include <algorithm>
#include <cstring>

#include "esp_log.h"

namespace probe {
namespace {

constexpr const char* kTag = "usb";

}  // namespace

esp_err_t UsbClient::start() {
    events_ = xQueueCreate(8, sizeof(UsbEvent));
    if (events_ == nullptr) return ESP_ERR_NO_MEM;

    usb_host_client_config_t config{};
    config.is_synchronous = false;
    config.max_num_event_msg = 8;
    config.async.client_event_callback = &UsbClient::onEvent;
    config.async.callback_arg = this;
    if (const esp_err_t err = usb_host_client_register(&config, &client_); err != ESP_OK)
        return err;

    return xTaskCreatePinnedToCore(&UsbClient::pump, "usb_client", 4096, this, 5, nullptr, 0) ==
                   pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

bool UsbClient::waitEvent(UsbEvent& event, TickType_t timeout) {
    return xQueueReceive(events_, &event, timeout) == pdTRUE;
}

void UsbClient::onEvent(const usb_host_client_event_msg_t* message, void* context) {
    auto& self = *static_cast<UsbClient*>(context);
    UsbEvent event;
    if (message->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        event.kind = UsbEvent::Kind::Attached;
        event.address = message->new_dev.address;
    } else if (message->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        event.kind = UsbEvent::Kind::Detached;
        event.device = message->dev_gone.dev_hdl;
    } else {
        return;
    }
    if (xQueueSend(self.events_, &event, 0) != pdTRUE)
        ESP_LOGW(kTag, "event queue full; dropped a hotplug event");
}

void UsbClient::pump(void* context) {
    auto& self = *static_cast<UsbClient*>(context);
    for (;;) usb_host_client_handle_events(self.client_, portMAX_DELAY);
}

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
        // The library has no per-transfer timeout yet: halting and flushing
        // the endpoint retires the transfer with CANCELED.
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

esp_err_t BulkPipe::write(std::span<const std::uint8_t> data, bool terminate,
                          std::uint32_t timeoutMs) {
    do {
        const std::size_t chunk = std::min(data.size(), capacity());
        std::memcpy(transfer_->data_buffer, data.data(), chunk);
        const bool last = chunk == data.size();
        const std::uint32_t flags = last && terminate ? USB_TRANSFER_FLAG_ZERO_PACK : 0U;
        if (const esp_err_t err = run(chunk, flags, timeoutMs); err != ESP_OK) return err;
        data = data.subspan(chunk);
    } while (!data.empty());
    return ESP_OK;
}

esp_err_t BulkPipe::read(std::size_t wanted, std::size_t& received, std::uint32_t timeoutMs) {
    // IN transfers must request whole packets; the device ends early with a
    // short packet when it has less.
    const std::size_t rounded = std::min(
        capacity(), static_cast<std::size_t>(usb_round_up_to_mps(static_cast<int>(
                        std::max<std::size_t>(wanted, 1)), maxPacket_)));
    received = 0;
    const esp_err_t err = run(rounded, 0U, timeoutMs);
    if (err == ESP_OK) received = static_cast<std::size_t>(transfer_->actual_num_bytes);
    return err;
}

esp_err_t readConfigDescriptor(usb_host_client_handle_t client, usb_device_handle_t device,
                               std::uint8_t index, std::vector<std::uint8_t>& descriptor) {
    constexpr std::uint16_t kMaxBytes = 2048;
    descriptor.clear();
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (done == nullptr) return ESP_ERR_NO_MEM;
    usb_transfer_t* transfer = nullptr;
    esp_err_t err = usb_host_transfer_alloc(USB_SETUP_PACKET_SIZE + kMaxBytes, 0, &transfer);
    if (err == ESP_OK) {
        auto* setup = reinterpret_cast<usb_setup_packet_t*>(transfer->data_buffer);
        USB_SETUP_PACKET_INIT_GET_CONFIG_DESC(setup, index, kMaxBytes);
        transfer->num_bytes = USB_SETUP_PACKET_SIZE + kMaxBytes;
        transfer->device_handle = device;
        transfer->bEndpointAddress = 0;
        transfer->callback = [](usb_transfer_t* t) {
            xSemaphoreGive(static_cast<SemaphoreHandle_t>(t->context));
        };
        transfer->context = done;
        err = usb_host_transfer_submit_control(client, transfer);
        if (err == ESP_OK) {
            // Control transfers are short; the device answers or the library
            // retires the transfer with an error.
            xSemaphoreTake(done, portMAX_DELAY);
            if (transfer->status != USB_TRANSFER_STATUS_COMPLETED ||
                transfer->actual_num_bytes < USB_SETUP_PACKET_SIZE + 4) {
                err = ESP_FAIL;
            } else {
                const std::uint8_t* bytes = transfer->data_buffer + USB_SETUP_PACKET_SIZE;
                descriptor.assign(bytes, bytes + transfer->actual_num_bytes - USB_SETUP_PACKET_SIZE);
            }
        }
        usb_host_transfer_free(transfer);
    }
    vSemaphoreDelete(done);
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

}  // namespace probe
