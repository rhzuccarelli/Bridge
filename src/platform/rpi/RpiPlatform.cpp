#include "aw/platform/rpi/RpiPlatform.hpp"

#include <chrono>
#include <thread>

#include "aw/core/Logger.hpp"
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
    hal::Platform platform;
    platform.clock = std::make_unique<SteadyClock>();
    platform.storage = std::make_unique<posix::PosixStorage>(config.projectsRoot);
    platform.display = std::make_unique<SpiDisplay>(config.display);
    platform.buttons = std::make_unique<GpioButtons>(config.pins);
    platform.encoder = std::make_unique<GpioEncoder>(config.pins);
    platform.audioOutput = std::make_unique<AlsaAudioOutput>(config.alsaDevice);
    platform.audioInput = nullptr;  // Phase 4+: capture for punch-in.
    platform.usb = std::make_unique<LibUsbHost>();
    platform.mtp = std::make_unique<LibMtpDevice>();

    if (!platform.isComplete()) {
        return core::makeError(core::ErrorCode::Unknown, "rpi platform incomplete");
    }

    core::log::info(kModule, "Raspberry Pi platform constructed");
    return platform;
}

}  // namespace aw::platform::rpi
