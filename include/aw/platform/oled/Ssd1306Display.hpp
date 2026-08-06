#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "aw/hal/DisplayInterface.hpp"
#include "aw/platform/oled/OledBus.hpp"

namespace aw::platform::oled {

/// Which controller sits behind the panel.
///
/// They are almost pin- and command-compatible, but two differences bite:
///  - the SH1106 has 132 columns of RAM for a 128-pixel panel, so its image is
///    offset by 2 columns; write to it as if it were an SSD1306 and everything
///    shifts right and wraps.
///  - the on-chip charge pump is enabled by a different command on each.
/// So the controller is a config choice, not something we can auto-detect over
/// a write-only SPI link.
enum class Controller : std::uint8_t { Ssd1306, Sh1106 };

/// 0 or 180 degrees. 90/270 would swap the canvas dimensions and need a
/// transpose on every flush; the panels are usually mounted one of these two
/// ways, so it is not worth the per-pixel cost. Handled by segment/COM remap in
/// the init sequence, which is free.
enum class Rotation : std::uint8_t { Normal, UpsideDown };

struct OledConfig {
    std::uint16_t width{128};
    std::uint16_t height{64};
    Controller controller{Controller::Ssd1306};
    Rotation rotation{Rotation::Normal};

    /// Column offset into controller RAM. Left at the sensible default for the
    /// controller (0 for SSD1306, 2 for SH1106) unless a particular module
    /// needs nudging.
    std::int8_t columnOffset{-1};  ///< -1 means "use the controller default".

    /// 0..255. Not really brightness on an OLED, but close enough for the UI.
    std::uint8_t contrast{0x8F};

    [[nodiscard]] std::int8_t effectiveColumnOffset() const noexcept {
        if (columnOffset >= 0) {
            return columnOffset;
        }
        return controller == Controller::Sh1106 ? 2 : 0;
    }
};

/// A monochrome SSD1306/SH1106 OLED as a `hal::DisplayInterface`.
///
/// The point of implementing the same interface as the colour panels is that
/// nothing above the HAL changes: the UI still renders into an RGB565
/// `display::Framebuffer` and `DisplayFlusher` still blits damaged regions here.
/// This class thresholds those RGB565 pixels down to one bit each and packs them
/// into the controller's page-oriented RAM. The UI does not know, and must not
/// need to know, that its colours have collapsed to on/off.
///
/// All the protocol lives here as pure logic over an `OledBus`; the transport is
/// injected, so the packing and the command stream are unit-tested against a
/// fake bus with no hardware present.
class Ssd1306Display final : public hal::DisplayInterface {
public:
    Ssd1306Display(OledConfig config, std::unique_ptr<OledBus> bus);
    ~Ssd1306Display() override;

    [[nodiscard]] hal::DisplayInfo info() const noexcept override;

    core::Status open() override;
    void close() noexcept override;
    core::Status blit(const hal::Rect& area, std::span<const std::uint16_t> pixels) override;
    core::Status present() override;

    /// Maps to the controller's contrast register - the nearest an OLED has to a
    /// backlight. 0 dims it to near-black; 1 is full.
    core::Status setBacklight(float level) override;

    /// Fills the whole panel with a recognisable geometric pattern (border,
    /// diagonal, filled corner) and pushes it. For bring-up: it proves the
    /// wiring, the controller choice and the orientation before any UI or font
    /// exists. Returns after one flush; the pattern stays until the next blit.
    core::Status showTestPattern();

    // --- Exposed for unit tests: the pure packing logic, no bus required. ----

    /// Sets or clears one bit in the page buffer. Out-of-range is ignored.
    void setPixel(std::int16_t x, std::int16_t y, bool on) noexcept;

    /// Reads one bit back. False when out of range.
    [[nodiscard]] bool pixel(std::int16_t x, std::int16_t y) const noexcept;

    /// The raw page buffer: `width * height/8` bytes, one bit per pixel, LSB is
    /// the top row of each 8-pixel page. This is exactly what is shipped to RAM.
    [[nodiscard]] std::span<const std::uint8_t> pageBuffer() const noexcept { return buffer_; }

    /// True once a non-black-vs-black threshold says this RGB565 pixel is lit.
    [[nodiscard]] static bool isLit(std::uint16_t rgb565) noexcept;

private:
    [[nodiscard]] std::vector<std::uint8_t> buildInitSequence() const;

    OledConfig config_;
    std::unique_ptr<OledBus> bus_;
    std::vector<std::uint8_t> buffer_;  ///< width * (height/8) bytes.
    bool open_{false};
    bool dirty_{true};
};

}  // namespace aw::platform::oled
