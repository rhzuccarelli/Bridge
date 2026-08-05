#pragma once

#include <memory>
#include <string>

#include "aw/hal/DisplayInterface.hpp"

namespace aw::platform::rpi {

/// Wiring and panel parameters for the prototype's LCD.
struct SpiDisplayConfig {
    std::string spiDevice{"/dev/spidev0.0"};
    int dataCommandLine{25};   ///< GPIO for the D/C pin.
    int resetLine{27};
    int backlightLine{18};     ///< PWM capable, so backlight is dimmable.
    std::uint32_t clockHz{40'000'000};
    std::uint16_t width{320};
    std::uint16_t height{240};
    std::uint8_t rotation{0};  ///< 0/90/180/270 as 0..3.
};

/// ST7789 / ILI9341 class panel over SPI.  (Phase 7.)
///
/// Writes RGB565 straight from the framebuffer's damage rectangle, using the
/// panel's column/row address window so only the changed region moves over the
/// bus.  Transfers go through spidev with DMA where the kernel offers it.
///
/// This is the class the ESP32-P4 port replaces with its LCD peripheral driver;
/// everything above `hal::DisplayInterface` stays exactly as it is.
class SpiDisplay final : public hal::DisplayInterface {
public:
    explicit SpiDisplay(SpiDisplayConfig config);
    ~SpiDisplay() override;

    [[nodiscard]] hal::DisplayInfo info() const noexcept override;

    core::Status open() override;
    void close() noexcept override;
    core::Status blit(const hal::Rect& area, std::span<const std::uint16_t> pixels) override;
    core::Status present() override;
    core::Status setBacklight(float level) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aw::platform::rpi
