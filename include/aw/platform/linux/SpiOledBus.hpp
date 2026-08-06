#pragma once

#include <cstdint>
#include <string>

#include "aw/platform/oled/OledBus.hpp"

namespace aw::platform::linuxfb {

/// Wiring for an SPI OLED on a Raspberry Pi.
///
/// Defaults match the recommended prototype wiring, chosen to sit clear of the
/// KY-040 encoder (GPIO 17/27/22) so both fit on a bare header:
///
///   SCK  -> pin 23  GPIO 11 (SPI0 SCLK)   hardware SPI
///   SDA  -> pin 19  GPIO 10 (SPI0 MOSI)   hardware SPI
///   CS   -> pin 24  GPIO 8  (SPI0 CE0)    driven by spidev automatically
///   DC   -> pin 18  GPIO 24               data/command select
///   RES  -> pin 22  GPIO 25               reset
///   VCC  -> pin 17  3.3 V
///   GND  -> pin 20  GND
///
/// SPI0 must be enabled (`dtparam=spi=on`), which creates /dev/spidev0.0.
struct SpiOledConfig {
    std::string spiDevice{"/dev/spidev0.0"};
    std::string gpioChip{"/dev/gpiochip0"};
    std::uint32_t dataCommandLine{24};  ///< BCM GPIO for D/C.
    std::uint32_t resetLine{25};        ///< BCM GPIO for RES.
    std::uint32_t speedHz{8'000'000};   ///< 8 MHz is comfortable for these panels.
};

/// `OledBus` over Linux spidev + the GPIO character device.
///
/// Uses only kernel uapi (`linux/spi/spidev.h`, `linux/gpio.h`) - no libgpiod,
/// no wiringPi, nothing to install. That keeps the dependency surface at zero
/// and means the code compiles anywhere with Linux headers, not only on the Pi.
///
/// The D/C line is driven per transaction; RES is pulsed once at `reset()`. CS
/// is left to the spidev driver, which asserts it around each write, so this
/// class never touches it.
class SpiOledBus final : public oled::OledBus {
public:
    explicit SpiOledBus(SpiOledConfig config);
    ~SpiOledBus() override;

    /// Opens the SPI device and requests the two GPIO lines as outputs. Must be
    /// called before any transfer; returns the reason it could not.
    core::Status openBus();

    core::Status reset() override;
    core::Status writeCommands(std::span<const std::uint8_t> commands) override;
    core::Status writeData(std::span<const std::uint8_t> data) override;

    /// True when /dev/spidev0.0 exists - a cheap pre-flight for backend selection.
    [[nodiscard]] static bool isAvailable(const std::string& spiDevice = "/dev/spidev0.0") noexcept;

private:
    core::Status setDataCommand(bool data);
    core::Status writeBytes(std::span<const std::uint8_t> bytes);

    SpiOledConfig config_;
    int spiFd_{-1};
    int lineFd_{-1};  ///< Request fd for the DC+RES line pair.
};

}  // namespace aw::platform::linuxfb
