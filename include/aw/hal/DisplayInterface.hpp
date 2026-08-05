#pragma once

#include <cstdint>
#include <span>

#include "aw/core/Result.hpp"

namespace aw::hal {

/// Rectangle in display pixels.  Signed so clipping arithmetic can go negative
/// before being intersected with the screen bounds.
struct Rect {
    std::int16_t x{0};
    std::int16_t y{0};
    std::int16_t width{0};
    std::int16_t height{0};

    [[nodiscard]] constexpr bool isEmpty() const noexcept { return width <= 0 || height <= 0; }
    [[nodiscard]] constexpr std::int32_t pixelCount() const noexcept {
        return isEmpty() ? 0 : static_cast<std::int32_t>(width) * height;
    }
};

struct DisplayInfo {
    std::uint16_t width{320};
    std::uint16_t height{240};
    bool supportsBacklightControl{false};
};

/// A dumb framebuffer target.
///
/// The interface accepts *pixels*, not drawing commands.  Everything above it
/// renders into an RGB565 buffer in RAM and hands over rectangles that changed.
///
/// Why this way:
///  - The panel is driven over SPI.  A command-per-primitive interface would
///    make every rounded corner a bus transaction; a rectangle blit is one DMA
///    transfer.
///  - RGB565 at 320x240 is 150 KiB.  That fits comfortably in Pi RAM and in
///    ESP32-P4 PSRAM, and it is the native format of every ST7789/ILI9341 class
///    panel, so no conversion happens on the way out.
///  - Damage rectangles, rather than full frames, are what keep the SPI budget
///    reasonable: a moving level meter touches a few hundred pixels, not 76800.
///
/// Rejected alternative: LVGL.  It solves a problem we do not have (rich,
/// pointer-driven widgets), costs flash and RAM we would rather spend on audio
/// buffers, and would have to be replaced wholesale on the ESP32 anyway.
class DisplayInterface {
public:
    virtual ~DisplayInterface() = default;

    DisplayInterface(const DisplayInterface&) = delete;
    DisplayInterface& operator=(const DisplayInterface&) = delete;

    [[nodiscard]] virtual DisplayInfo info() const noexcept = 0;

    virtual core::Status open() = 0;
    virtual void close() noexcept = 0;

    /// Copies `pixels` into `area`.  `pixels` is tightly packed, row major,
    /// `area.width * area.height` entries, RGB565 in host byte order (the
    /// backend byte-swaps if its panel needs big endian).
    virtual core::Status blit(const Rect& area, std::span<const std::uint16_t> pixels) = 0;

    /// Makes previously blitted content visible.  A no-op on panels that update
    /// continuously; meaningful on double buffered ones.
    virtual core::Status present() = 0;

    /// 0.0 = off, 1.0 = full.  Fails with NotImplemented where unsupported.
    virtual core::Status setBacklight(float level) = 0;

protected:
    DisplayInterface() = default;
};

}  // namespace aw::hal
