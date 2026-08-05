#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "aw/display/Color.hpp"
#include "aw/hal/DisplayInterface.hpp"

namespace aw::display {

/// An off-screen RGB565 image with damage tracking.
///
/// Screens draw here; the damage rectangle records what actually changed; the
/// flusher sends only that region over SPI.  On a 320x240 panel at ~20 MHz SPI
/// a full frame costs roughly 60 ms - far too slow for a moving meter.  Tracking
/// damage turns that into a couple of milliseconds for the parts that moved.
///
/// A single union-of-changes rectangle, not a list of them: a list is more
/// precise but a UI made of a few boxes rarely benefits, and one rectangle keeps
/// the flush loop trivial.
class Framebuffer {
public:
    Framebuffer(std::uint16_t width, std::uint16_t height);

    [[nodiscard]] std::uint16_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint16_t height() const noexcept { return height_; }

    [[nodiscard]] std::span<Color> pixels() noexcept { return pixels_; }
    [[nodiscard]] std::span<const Color> pixels() const noexcept { return pixels_; }

    /// Bounds-checked single pixel write; marks damage.
    void setPixel(std::int16_t x, std::int16_t y, Color color) noexcept;

    void fill(Color color) noexcept;

    /// Row `y`, clipped to the buffer.  For the drawing primitives in `Painter`.
    [[nodiscard]] std::span<Color> row(std::uint16_t y) noexcept;

    // --- Damage tracking -----------------------------------------------------
    void markDamaged(const hal::Rect& area) noexcept;
    [[nodiscard]] hal::Rect damage() const noexcept { return damage_; }
    [[nodiscard]] bool hasDamage() const noexcept { return !damage_.isEmpty(); }
    void clearDamage() noexcept;

    /// Copies the damaged region out into `destination`, tightly packed, and
    /// returns the rectangle it covers.  Used by the flusher; `destination`
    /// must hold at least `damage().pixelCount()` entries.
    hal::Rect extractDamage(std::span<Color> destination) const;

private:
    std::uint16_t width_;
    std::uint16_t height_;
    std::vector<Color> pixels_;
    hal::Rect damage_{};
};

}  // namespace aw::display
