#include "aw/platform/rpi/RpiPlatform.hpp"

#include <chrono>
#include <cstdlib>
#include <thread>

#include "aw/core/Logger.hpp"
#include "aw/platform/CompositeButtons.hpp"
#include "aw/platform/desktop/DevBackends.hpp"
#include "aw/platform/posix/PosixStorage.hpp"
#include "aw/platform/rpi/AlsaAudioOutput.hpp"
#include "aw/platform/rpi/LibMtpDevice.hpp"
#include "aw/platform/rpi/LibUsbHost.hpp"

namespace aw::platform::rpi {
namespace {

constexpr std::string_view kModule = "rpi";

class SteadyClock final : public hal::ClockInterface {
public:
    std::uint64_t nowMicros() const noexcept override {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(now).count());
    }

    void sleepMicros(std::uint64_t micros) override {
        std::this_thread::sleep_for(std::chrono::microseconds{micros});
    }
};

}  // namespace

core::Result<hal::Platform> createPlatform(const RpiConfig& config) {
    RpiConfig effective = config;
    if (const char* chip = std::getenv("AW_GPIO_CHIP")) {
        effective.pins.chipName = chip;  // See GpioPinMap::chipName for why.
    }

    hal::Platform platform;
    platform.clock = std::make_unique<SteadyClock>();
    platform.storage = std::make_unique<posix::PosixStorage>(effective.projectsRoot);
    platform.audioOutput = std::make_unique<AlsaAudioOutput>(effective.alsaDevice);
    platform.audioInput = nullptr;  // Phase 4+: capture for punch-in.
    platform.usb = std::make_unique<LibUsbHost>();
    platform.mtp = std::make_unique<LibMtpDevice>();

    // Display and input: the prototype drives an HDMI panel through the Linux
    // framebuffer and takes its non-encoder buttons from the touch strip, so it
    // uses the same development backends as the simulator assembly.  The real
    // encoder is GPIO and is merged in alongside them.
    desktop::DevBackends backends = desktop::createDevBackends(desktop::optionsFromEnvironment());
    platform.display = std::move(backends.display);

    auto buttons = std::make_unique<CompositeButtons>();
    buttons->addSource(std::make_unique<GpioButtons>(effective.pins));
    buttons->addSource(std::move(backends.buttons));
    platform.buttons = std::move(buttons);

    // The KY-040 is the real control; the keyboard encoder stays available as a
    // fallback only when there is no GPIO encoder to be had.
    platform.encoder = std::make_unique<GpioEncoder>(effective.pins);

    if (!platform.isComplete()) {
        return core::makeError(core::ErrorCode::Unknown, "rpi platform incomplete");
    }

    core::log::info(kModule, "Raspberry Pi platform ready: " + backends.description +
                                 " + GPIO (" + effective.pins.chipName + ")");
    return platform;
}

}  // namespace aw::platform::rpi
