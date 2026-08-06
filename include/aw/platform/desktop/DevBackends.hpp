#pragma once

#include <memory>
#include <string>

#include "aw/hal/ButtonInterface.hpp"
#include "aw/hal/DisplayInterface.hpp"
#include "aw/hal/EncoderInterface.hpp"

namespace aw::platform::desktop {

/// Which backends to prefer.  "auto" picks the best thing actually present.
struct DevBackendOptions {
    std::string display{"auto"};    ///< auto | oled | fb | terminal | memory
    std::string input{"auto"};      ///< auto | touch | keyboard | none  (comma separated)
    std::string framebufferDevice{"/dev/fb0"};
    std::string touchDevice;        ///< Empty scans /dev/input/event*.
    std::uint16_t canvasWidth{320};
    std::uint16_t canvasHeight{240};
    bool stretch{false};
    std::uint16_t fallbackPanelWidth{480};   ///< Used when the panel cannot be queried.
    std::uint16_t fallbackPanelHeight{320};

    // SPI OLED (SSD1306 / SH1106). Only consulted when the OLED is selected.
    std::string oledSpiDevice{"/dev/spidev0.0"};
    std::string oledController{"ssd1306"};   ///< ssd1306 | sh1106
    std::uint16_t oledWidth{128};
    std::uint16_t oledHeight{64};
    bool oledUpsideDown{false};
};

/// Reads the options from the environment:
///   AW_DISPLAY, AW_INPUT, AW_FB_DEVICE, AW_TOUCH_DEVICE, AW_STRETCH
///
/// Environment rather than a config file, deliberately: this is developer
/// scaffolding whose selection changes between one run and the next
/// ("`AW_DISPLAY=terminal` over SSH, `fb` on the bench"), and a file would just
/// be state to forget you had set.
[[nodiscard]] DevBackendOptions optionsFromEnvironment();

/// The display and input backends chosen for a development run.
///
/// `buttons` is always a `CompositeButtons`, even with a single source, so the
/// caller never has to care how many input devices were found.
struct DevBackends {
    std::unique_ptr<hal::DisplayInterface> display;
    std::unique_ptr<hal::ButtonInterface> buttons;
    std::unique_ptr<hal::EncoderInterface> encoder;
    std::string description;  ///< Human readable summary, for the startup log.
};

/// Builds them, degrading gracefully: a missing framebuffer falls back to the
/// terminal, a missing terminal to an in-memory display, and a missing
/// touchscreen simply contributes no button source.  Never fails - a
/// development build that refuses to start because a peripheral is absent is
/// useless precisely when you need it.
[[nodiscard]] DevBackends createDevBackends(const DevBackendOptions& options);

}  // namespace aw::platform::desktop
