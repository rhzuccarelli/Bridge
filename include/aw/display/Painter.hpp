#pragma once

#include <span>
#include <string_view>
#include <utility>

#include "aw/display/Color.hpp"
#include "aw/display/Font.hpp"
#include "aw/display/Framebuffer.hpp"
#include "aw/hal/DisplayInterface.hpp"

namespace aw::display {

enum class TextAlign : std::uint8_t { Left, Center, Right };

/// Drawing primitives over a `Framebuffer`.
///
/// A thin, stateless-ish helper rather than a retained scene graph: screens draw
/// themselves from scratch each time they are dirty, which means there is no
/// tree to keep in sync with the model and no "why is this widget stale" class
/// of bug.  Redrawing a 320x240 screen of flat rectangles costs well under a
/// millisecond, so the retained-mode complexity buys nothing.
///
/// Every primitive clips to the framebuffer and reports damage automatically.
class Painter {
public:
    explicit Painter(Framebuffer& target) noexcept : target_{target} {}

    [[nodiscard]] Framebuffer& target() noexcept { return target_; }

    void clear(Color color) noexcept;
    void fillRect(const hal::Rect& area, Color color) noexcept;
    void drawRect(const hal::Rect& area, Color color) noexcept;
    void drawHLine(std::int16_t x, std::int16_t y, std::int16_t length, Color color) noexcept;
    void drawVLine(std::int16_t x, std::int16_t y, std::int16_t length, Color color) noexcept;

    /// Returns the width drawn, so callers can lay out following text.
    std::int16_t drawText(std::int16_t x,
                          std::int16_t y,
                          std::string_view text,
                          const Font& font,
                          Color color) noexcept;

    void drawTextIn(const hal::Rect& area,
                    std::string_view text,
                    const Font& font,
                    Color color,
                    TextAlign align) noexcept;

    /// Draws a min/max envelope: `peaks` holds one pair per horizontal pixel,
    /// each in [-1, 1].  Pre-computed by the waveform cache, never by scanning
    /// the audio file at draw time.
    void drawWaveform(const hal::Rect& area,
                      std::span<const std::pair<float, float>> peaks,
                      Color color) noexcept;

    /// Horizontal or vertical level meter.  `level` is linear in [0, 1];
    /// the colour zones come from the palette.
    void drawMeter(const hal::Rect& area, float level, bool vertical) noexcept;

private:
    Framebuffer& target_;
};

}  // namespace aw::display
