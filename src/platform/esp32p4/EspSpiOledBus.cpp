#include "aw/platform/esp32p4/EspSpiOledBus.hpp"

#include <algorithm>
#include <string>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace aw::platform::esp32p4 {
namespace {

constexpr spi_host_device_t kHost = SPI2_HOST;
constexpr int kMaxTransferBytes = 1024;  // A whole 128x64 frame.

core::Status espError(esp_err_t err, const char* what) {
    return core::makeError(core::ErrorCode::IoError,
                           std::string{what} + ": " + esp_err_to_name(err));
}

core::Status outputPin(int pin, int level) {
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << static_cast<unsigned>(pin);
    config.mode = GPIO_MODE_OUTPUT;
    if (const esp_err_t err = gpio_config(&config); err != ESP_OK) {
        return espError(err, "OLED control GPIO");
    }
    gpio_set_level(static_cast<gpio_num_t>(pin), static_cast<std::uint32_t>(level));
    return core::success();
}

}  // namespace

EspOledPins EspOledPins::picoPrototype() noexcept {
    EspOledPins pins;
    pins.sck = 6;
    pins.mosi = 5;
    pins.reset = 4;
    pins.dataCommand = 3;
    pins.chipSelect = 14;
    return pins;
}

EspSpiOledBus::EspSpiOledBus(EspOledPins pins) : pins_{pins} {}

EspSpiOledBus::~EspSpiOledBus() {
    if (device_ != nullptr) spi_bus_remove_device(device_);
    if (busOwned_) spi_bus_free(kHost);
}

core::Status EspSpiOledBus::open() {
    if (device_ != nullptr) return core::success();
    if (auto status = outputPin(pins_.dataCommand, 0); !status) return status;
    if (auto status = outputPin(pins_.reset, 1); !status) return status;

    spi_bus_config_t bus{};
    bus.mosi_io_num = pins_.mosi;
    bus.miso_io_num = -1;
    bus.sclk_io_num = pins_.sck;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = kMaxTransferBytes;
    if (const esp_err_t err = spi_bus_initialize(kHost, &bus, SPI_DMA_CH_AUTO); err != ESP_OK) {
        return espError(err, "spi_bus_initialize");
    }
    busOwned_ = true;

    spi_device_interface_config_t device{};
    device.mode = 0;
    device.clock_speed_hz = static_cast<int>(pins_.speedHz);
    device.spics_io_num = pins_.chipSelect;
    device.queue_size = 1;
    if (const esp_err_t err = spi_bus_add_device(kHost, &device, &device_); err != ESP_OK) {
        device_ = nullptr;
        return espError(err, "spi_bus_add_device");
    }
    return core::success();
}

core::Status EspSpiOledBus::reset() {
    if (auto status = open(); !status) return status;
    // Datasheet minimum is 3 us low; generous holds cost nothing at boot.
    gpio_set_level(static_cast<gpio_num_t>(pins_.reset), 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(static_cast<gpio_num_t>(pins_.reset), 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    return core::success();
}

core::Status EspSpiOledBus::writeCommands(std::span<const std::uint8_t> commands) {
    return write(commands, false);
}

core::Status EspSpiOledBus::writeData(std::span<const std::uint8_t> data) {
    return write(data, true);
}

core::Status EspSpiOledBus::write(std::span<const std::uint8_t> bytes, bool data) {
    if (bytes.empty()) return core::success();
    if (auto status = open(); !status) return status;
    gpio_set_level(static_cast<gpio_num_t>(pins_.dataCommand), data ? 1U : 0U);
    while (!bytes.empty()) {
        const std::size_t chunk = std::min<std::size_t>(bytes.size(), kMaxTransferBytes);
        spi_transaction_t transaction{};
        transaction.length = chunk * 8;
        transaction.tx_buffer = bytes.data();
        if (const esp_err_t err = spi_device_polling_transmit(device_, &transaction);
            err != ESP_OK) {
            return espError(err, "OLED SPI write");
        }
        bytes = bytes.subspan(chunk);
    }
    return core::success();
}

}  // namespace aw::platform::esp32p4
