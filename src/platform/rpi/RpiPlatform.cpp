#include "aw/platform/rpi/RpiPlatform.hpp"

#include <chrono>
#include <cstdlib>
#include <thread>

#include "aw/core/Logger.hpp"
#include "aw/platform/CompositeButtons.hpp"
#include "aw/platform/CompositeEncoder.hpp"
#include "aw/platform/FallbackAudioOutput.hpp"
#include "aw/platform/NullAudioOutput.hpp"
#include "aw/platform/desktop/DevBackends.hpp"
#include "aw/platform/posix/PosixStorage.hpp"
#include "aw/platform/rpi/AlsaAudioOutput.hpp"
#include "aw/platform/rpi/AlsaTp7ModeSwitcher.hpp"
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
    if (const char* device = std::getenv("AW_ALSA_DEVICE")) {
        effective.alsaDevice = device;
    }
    if (const char* device = std::getenv("AW_MIDI_DEVICE")) {
        effective.midiDevice = device;
    }

    hal::Platform platform;
    platform.clock = std::make_unique<SteadyClock>();
    platform.storage = std::make_unique<posix::PosixStorage>(effective.projectsRoot);
    platform.audioInput = nullptr;  // Phase 4+: capture for punch-in.
    platform.usb = std::make_unique<LibUsbHost>();
    platform.mtp = std::make_unique<LibMtpDevice>();
    platform.deviceMode = std::make_unique<AlsaTp7ModeSwitcher>(effective.midiDevice);

    // Prefer exact 48 kHz / S24_3LE ALSA playback. A disconnected interface
    // falls back to the wall-clock silent backend so projects and UI remain
    // usable; the fallback never performs implicit resampling.
    platform.audioOutput = std::make_unique<FallbackAudioOutput>(
        std::make_unique<AlsaAudioOutput>(effective.alsaDevice),
        std::make_unique<NullAudioOutput>());

    // Display and input come from the shared development backends (OLED or HDMI
    // panel, plus keyboard). The real GPIO encoder and button lines are merged
    // in alongside them, so nothing is lost when a GPIO driver is still a stub.
    desktop::DevBackends backends = desktop::createDevBackends(desktop::optionsFromEnvironment());
    platform.display = std::move(backends.display);

    auto buttons = std::make_unique<CompositeButtons>();
    buttons->addSource(std::make_unique<GpioButtons>(effective.pins));
    buttons->addSource(std::move(backends.buttons));  // keyboard (+ touch on HDMI)
    platform.buttons = std::move(buttons);

    // The KY-040 is the real control. Keep the keyboard encoder composed beside
    // it so bench work remains possible over SSH; motion from both simply sums.
    auto encoder = std::make_unique<CompositeEncoder>();
    encoder->addSource(std::make_unique<GpioEncoder>(effective.pins));
    if (backends.encoder) {
        encoder->addSource(std::move(backends.encoder));
    }
    platform.encoder = std::move(encoder);

    if (!platform.isComplete()) {
        return core::makeError(core::ErrorCode::Unknown, "rpi platform incomplete");
    }

    core::log::info(kModule, "Raspberry Pi platform ready: " + backends.description +
                                 " + GPIO (" + effective.pins.chipName + ")");
    core::log::info(kModule, "ALSA playback device: " + effective.alsaDevice);
    core::log::info(kModule, "TP-7 MIDI device: " + effective.midiDevice);
    return platform;
}

}  // namespace aw::platform::rpi
