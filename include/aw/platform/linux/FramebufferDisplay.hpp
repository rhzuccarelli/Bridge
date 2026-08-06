#pragma once

#include <memory>
#include <optional>
#include <string>

#include "aw/hal/DisplayInterface.hpp"
#include "aw/platform/desktop/VirtualPanel.hpp"

namespace aw::platform::linuxfb {

struct FramebufferConfig {
    std::string device{"/dev/fb0"};

    /// Logical UI size.  Fixed at the shipping device's panel size so layouts
    /// designed now do not need reworking for the ESP32-P4.
    std::uint16_t canvasWidth{320};
    std::uint16_t canvasHeight{240};

    /// Draw the virtual control panel in the space around the canvas.
    bool drawVirtualPanel{true};

    /// Scale the canvas to fill the screen instead of letterboxing it.
    /// Off by default - see the note above about layout rework.
    bool stretch{false};
};

/// `DisplayInterface` over the Linux framebuffer (`/dev/fb0`).
///
/// This is what drives an HDMI panel such as the 3.5" 480x320 LCD: from the
/// Pi's point of view that display is an ordinary monitor, so there is no SPI
/// panel driver and no controller init sequence involved - just a memory
/// mapping the kernel already set up.  Which makes it both the simplest backend
/// to write and the most portable one: it works on any Linux machine with a
/// framebuffer, including a developer's VT.
///
/// The class reports the *logical* canvas size through `info()`, and maps blits
/// into the physical panel itself.  Everything above therefore draws for a
/// 320x240 device no matter what is plugged in.
///
/// Supports 16 bpp (RGB565, a straight copy) and 32 bpp (RGB8888, converted per
/// pixel).  Other depths are refused rather than guessed at.
class FramebufferDisplay final : public hal::DisplayInterface {
public:
    /// `layout` is optional.  Pass the same one given to the touch backend so
    /// the drawn keys and the hit-tested keys cannot drift apart; omit it and
    /// the display derives its own at `open()`.
    explicit FramebufferDisplay(FramebufferConfig config,
                                std::optional<desktop::VirtualPanelLayout> layout = std::nullopt);
    ~FramebufferDisplay() override;

    [[nodiscard]] hal::DisplayInfo info() const noexcept override;

    core::Status open() override;
    void close() noexcept override;
    core::Status blit(const hal::Rect& area, std::span<const std::uint16_t> pixels) override;
    core::Status present() override;
    core::Status setBacklight(float level) override;

    /// Physical panel size, once open.  Zero before that.
    [[nodiscard]] std::uint16_t panelWidth() const noexcept;
    [[nodiscard]] std::uint16_t panelHeight() const noexcept;

    /// The layout used for the virtual control panel, so the touch backend can
    /// hit-test against exactly what was drawn.  Null until `open()` succeeds.
    [[nodiscard]] const desktop::VirtualPanelLayout* panelLayout() const noexcept;

    /// True when the framebuffer device exists and is writable.
    [[nodiscard]] static bool isAvailable(const std::string& device = "/dev/fb0") noexcept;

    struct PanelSize {
        std::uint16_t width{0};
        std::uint16_t height{0};
    };

    /// Reads the panel geometry without taking the device over, so a caller can
    /// build the shared `VirtualPanelLayout` before anything is opened.
    [[nodiscard]] static std::optional<PanelSize> queryPanelSize(
        const std::string& device = "/dev/fb0") noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aw::platform::linuxfb
