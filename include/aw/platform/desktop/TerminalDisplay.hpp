#pragma once

#include <memory>

#include "aw/hal/DisplayInterface.hpp"

namespace aw::platform::desktop {

/// Renders the real framebuffer into a terminal, using ANSI truecolour
/// half-block characters.
///
/// This is a `DisplayInterface` like any other, which is the whole point: the UI
/// code above it does not know it is drawing to a terminal rather than an SPI
/// panel, so every pixel this shows is a pixel the real panel would show.  It is
/// a *preview*, not a mock - a layout bug visible here is a layout bug on the
/// hardware.
///
/// Each terminal cell carries two vertical pixels (U+2580 "upper half block",
/// foreground = top pixel, background = bottom), so a cell is roughly square.
/// The frame is downscaled by an integer factor chosen from the terminal size,
/// nearest-neighbour: at 320x240 in a 110x32 terminal that is a factor of 4,
/// giving an 80x60 preview.  Nearest rather than averaging on purpose - it keeps
/// one-pixel lines visible instead of blurring them away, and one-pixel lines
/// are most of this UI.
///
/// Falls back gracefully: if stdout is not a terminal the class still accepts
/// blits and simply does not draw, so redirecting output never breaks a run.
class TerminalDisplay final : public hal::DisplayInterface {
public:
    TerminalDisplay(std::uint16_t width, std::uint16_t height);
    ~TerminalDisplay() override;

    [[nodiscard]] hal::DisplayInfo info() const noexcept override;

    core::Status open() override;
    void close() noexcept override;
    core::Status blit(const hal::Rect& area, std::span<const std::uint16_t> pixels) override;
    core::Status present() override;
    core::Status setBacklight(float level) override;

    /// True when stdout is a terminal capable of showing the preview.
    [[nodiscard]] static bool isAvailable() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aw::platform::desktop
