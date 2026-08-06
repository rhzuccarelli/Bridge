#include "aw/platform/desktop/DevBackends.hpp"

#include <cstdlib>
#include <vector>

#include "aw/platform/CompositeButtons.hpp"
#include "aw/platform/desktop/KeyboardInput.hpp"
#include "aw/platform/desktop/TerminalDisplay.hpp"
#include "aw/platform/desktop/VirtualPanel.hpp"
#include "aw/platform/linux/EvdevTouch.hpp"
#include "aw/platform/linux/FramebufferDisplay.hpp"

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
    return options;
}

DevBackends createDevBackends(const DevBackendOptions& options) {
    DevBackends backends;
    auto buttons = std::make_unique<CompositeButtons>();

    const bool framebufferUsable =
        wants(options.display, "fb") &&
        linuxfb::FramebufferDisplay::isAvailable(options.framebufferDevice);

    if (framebufferUsable) {
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
