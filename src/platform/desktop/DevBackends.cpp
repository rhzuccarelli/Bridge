#include "aw/platform/desktop/DevBackends.hpp"

#include <cstdlib>
#include <vector>

#include "aw/platform/CompositeButtons.hpp"
#include "aw/platform/desktop/KeyboardInput.hpp"
#include "aw/platform/desktop/TerminalDisplay.hpp"
#include "aw/platform/desktop/VirtualPanel.hpp"
#include "aw/platform/linux/EvdevTouch.hpp"
#include "aw/platform/linux/FramebufferDisplay.hpp"
#include "aw/platform/linux/SpiOledBus.hpp"
#include "aw/platform/oled/Ssd1306Display.hpp"

namespace aw::platform::desktop {
namespace {

std::string environmentOr(const char* name, std::string fallback) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string{value} : std::move(fallback);
}

bool wants(const std::string& preference, std::string_view backend) {
    return preference == "auto" || preference.find(backend) != std::string::npos;
}

/// A display that accepts frames and shows nothing.  The last resort, so a
/// headless CI run still exercises the full application loop.
class MemoryDisplay final : public hal::DisplayInterface {
public:
    MemoryDisplay(std::uint16_t width, std::uint16_t height) : info_{width, height, false} {}

    hal::DisplayInfo info() const noexcept override { return info_; }
    core::Status open() override { return core::success(); }
    void close() noexcept override {}
    core::Status blit(const hal::Rect&, std::span<const std::uint16_t>) override {
        return core::success();
    }
    core::Status present() override { return core::success(); }
    core::Status setBacklight(float) override {
        return core::notImplemented("memory display has no backlight");
    }

private:
    hal::DisplayInfo info_;
};

class NullEncoder final : public hal::EncoderInterface {
public:
    void setListener(hal::EncoderListener*) noexcept override {}
    core::Status start() override { return core::success(); }
    void stop() noexcept override {}
    std::int32_t consumeDelta() noexcept override { return 0; }
};

}  // namespace

DevBackendOptions optionsFromEnvironment() {
    DevBackendOptions options;
    options.display = environmentOr("AW_DISPLAY", "auto");
    options.input = environmentOr("AW_INPUT", "auto");
    options.framebufferDevice = environmentOr("AW_FB_DEVICE", "/dev/fb0");
    options.touchDevice = environmentOr("AW_TOUCH_DEVICE", "");
    options.stretch = environmentOr("AW_STRETCH", "0") == "1";

    options.oledSpiDevice = environmentOr("AW_OLED_SPI", "/dev/spidev0.0");
    options.oledController = environmentOr("AW_OLED_CHIP", "ssd1306");
    options.oledUpsideDown = environmentOr("AW_OLED_ROTATE", "0") == "180";
    return options;
}

namespace {

/// Builds the OLED display if it is wanted and the SPI bus opens.  Returns null
/// (and leaves `description` untouched) otherwise, so the caller falls through
/// to the next display option - exactly like the framebuffer branch.
std::unique_ptr<hal::DisplayInterface> tryOled(const DevBackendOptions& options,
                                               std::string& description) {
    // Explicit only: an OLED cannot be probed over its write-only SPI link, and
    // /dev/spidev0.0 exists on any SPI-enabled Pi whether or not a panel is
    // attached. So "auto" must never silently pick it over a detectable HDMI panel.
    if (options.display.find("oled") == std::string::npos) {
        return nullptr;
    }
    if (!linuxfb::SpiOledBus::isAvailable(options.oledSpiDevice)) {
        return nullptr;  // No /dev/spidevN.N - SPI not enabled, or not a Pi.
    }

    auto bus = std::make_unique<linuxfb::SpiOledBus>(linuxfb::SpiOledConfig{options.oledSpiDevice});
    if (auto status = bus->openBus(); !status) {
        return nullptr;  // Present but unusable; a warning is logged by the app.
    }

    oled::OledConfig config;
    config.width = options.oledWidth;
    config.height = options.oledHeight;
    config.controller = options.oledController == "sh1106" ? oled::Controller::Sh1106
                                                           : oled::Controller::Ssd1306;
    config.rotation = options.oledUpsideDown ? oled::Rotation::UpsideDown : oled::Rotation::Normal;

    description = "OLED " + std::to_string(options.oledWidth) + "x" +
                 std::to_string(options.oledHeight) + " (" + options.oledController + ")";
    return std::make_unique<oled::Ssd1306Display>(config, std::move(bus));
}

}  // namespace

DevBackends createDevBackends(const DevBackendOptions& options) {
    DevBackends backends;
    auto buttons = std::make_unique<CompositeButtons>();

    // The OLED is tried first: it is an explicit choice (small monochrome
    // panel), so when the user asks for it and it is present, nothing should
    // silently prefer a different screen.
    if (auto display = tryOled(options, backends.description)) {
        backends.display = std::move(display);
    }

    const bool framebufferUsable =
        !backends.display &&
        wants(options.display, "fb") &&
        linuxfb::FramebufferDisplay::isAvailable(options.framebufferDevice);

    if (backends.display) {
        // Already chosen (OLED). Nothing to do here.
    } else if (framebufferUsable) {
        // Build the layout once, from the real panel geometry, and give the same
        // value to the display and the touch backend.
        const auto panel = linuxfb::FramebufferDisplay::queryPanelSize(options.framebufferDevice);
        const std::uint16_t panelWidth = panel ? panel->width : options.fallbackPanelWidth;
        const std::uint16_t panelHeight = panel ? panel->height : options.fallbackPanelHeight;

        const VirtualPanelLayout layout{panelWidth, panelHeight, options.canvasWidth,
                                        options.canvasHeight};

        linuxfb::FramebufferConfig displayConfig;
        displayConfig.device = options.framebufferDevice;
        displayConfig.canvasWidth = options.canvasWidth;
        displayConfig.canvasHeight = options.canvasHeight;
        displayConfig.stretch = options.stretch;
        backends.display = std::make_unique<linuxfb::FramebufferDisplay>(displayConfig, layout);
        backends.description = "framebuffer " + options.framebufferDevice;

        if (wants(options.input, "touch") && !layout.keys().empty()) {
            linuxfb::TouchConfig touchConfig;
            touchConfig.device = options.touchDevice;
            touchConfig.panelWidth = panelWidth;
            touchConfig.panelHeight = panelHeight;
            buttons->addSource(std::make_unique<linuxfb::EvdevTouch>(touchConfig, layout));
            backends.description += " + touch keys";
        }
    } else if (wants(options.display, "terminal") && TerminalDisplay::isAvailable()) {
        backends.display =
            std::make_unique<TerminalDisplay>(options.canvasWidth, options.canvasHeight);
        backends.description = "terminal preview";
    } else {
        backends.display =
            std::make_unique<MemoryDisplay>(options.canvasWidth, options.canvasHeight);
        backends.description = "headless";
    }

    if (wants(options.input, "keyboard") && KeyboardInputHub::isAvailable()) {
        auto hub = std::make_shared<KeyboardInputHub>();
        buttons->addSource(std::make_unique<KeyboardButtons>(hub));
        backends.encoder = std::make_unique<KeyboardEncoder>(hub);
        backends.description += " + keyboard";
    } else {
        backends.encoder = std::make_unique<NullEncoder>();
    }

    backends.buttons = std::move(buttons);
    return backends;
}

}  // namespace aw::platform::desktop
