#pragma once

#include <cstdint>
#include <span>

#include "aw/platform/oled/OledBus.hpp"

struct spi_device_t;  // ESP-IDF; spi_device_handle_t is spi_device_t*.

namespace aw::platform::esp32p4 {

/// Which GPIOs the 7-pin SPI OLED module sits on.
struct EspOledPins {
    int sck{-1};
    int mosi{-1};  ///< Labelled SDA on the module.
    int reset{-1};
    int dataCommand{-1};
    int chipSelect{-1};
    std::uint32_t speedHz{8'000'000};  ///< As on the Pi.

    /// The Waveshare ESP32-P4-Pico prototype: SCK GPIO6, SDA GPIO5, RES GPIO4,
    /// DC GPIO3, CS GPIO14; VCC on 3.3 V.
    [[nodiscard]] static EspOledPins picoPrototype() noexcept;
};

/// `oled::OledBus` over the ESP-IDF SPI master (SPI2, mode 0), with D/C and
/// RES on plain GPIOs. The P4 counterpart of `linuxfb::SpiOledBus`: it moves
/// bytes and nothing else; the SSD1306 protocol stays in `Ssd1306Display`.
///
/// Transfers are polled, not queued: a full frame is 8 pages of 128 bytes,
/// about 1 ms at 8 MHz, and polling keeps D/C changes trivially in order with
/// the bytes they qualify.
class EspSpiOledBus final : public oled::OledBus {
public:
    explicit EspSpiOledBus(EspOledPins pins);
    ~EspSpiOledBus() override;

    core::Status reset() override;
    core::Status writeCommands(std::span<const std::uint8_t> commands) override;
    core::Status writeData(std::span<const std::uint8_t> data) override;

private:
    core::Status open();
    core::Status write(std::span<const std::uint8_t> bytes, bool data);

    EspOledPins pins_;
    spi_device_t* device_{nullptr};
    bool busOwned_{false};
};

}  // namespace aw::platform::esp32p4
