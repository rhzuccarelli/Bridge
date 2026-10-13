#include "aw/platform/esp32p4/Esp32P4Platform.hpp"

#include <unistd.h>

#include <memory>
#include <vector>

#include "aw/core/Logger.hpp"
#include "aw/platform/NullAudioOutput.hpp"
#include "aw/platform/esp32p4/EspGpioInput.hpp"
#include "aw/platform/esp32p4/EspSpiOledBus.hpp"
#include "aw/platform/esp32p4/EspUsb.hpp"
#include "aw/platform/esp32p4/SerialPreviewDisplay.hpp"
#include "esp_timer.h"

namespace aw::platform::esp32p4 {
namespace {

constexpr std::string_view kModule = "esp32p4";

class EspClock final : public hal::ClockInterface {
public:
    std::uint64_t nowMicros() const noexcept override {
        return static_cast<std::uint64_t>(esp_timer_get_time());
    }

    // usleep yields to FreeRTOS for whole ticks and busy-waits only the
    // remainder, so CONFIG_FREERTOS_HZ sets how precise the UI frame rate is.
    void sleepMicros(std::uint64_t micros) override {
        usleep(static_cast<useconds_t>(micros));
    }
};

/// An empty volume: the projects root exists and lists nothing; anything that
/// would read or write a file fails as "not connected". The UI shows an empty
/// project list, which is what it shows for a blank card anyway.
class NoCardStorage final : public hal::StorageInterface {
public:
    explicit NoCardStorage(core::Path root) : root_{std::move(root)} {}

    core::Result<std::unique_ptr<hal::FileHandle>> open(const core::Path& /*path*/,
                                                        hal::OpenMode /*mode*/) override {
        return noCard();
    }
    bool exists(const core::Path& path) const override { return path == root_; }
    bool isDirectory(const core::Path& path) const override { return path == root_; }
    core::Status createDirectories(const core::Path& /*path*/) override { return noCard(); }
    core::Status remove(const core::Path& /*path*/) override { return noCard(); }
    core::Status rename(const core::Path& /*from*/, const core::Path& /*to*/) override {
        return noCard();
    }
    core::Result<std::vector<hal::DirectoryEntry>> list(const core::Path& directory) const override {
        if (directory == root_) return std::vector<hal::DirectoryEntry>{};
        return noCard();
    }
    core::Result<std::uint64_t> freeSpaceBytes(const core::Path& /*path*/) const override {
        return std::uint64_t{0};
    }
    core::Path projectsRoot() const override { return root_; }

private:
    static core::Error noCard() {
        return core::makeError(core::ErrorCode::DeviceNotConnected, "no SD card support yet");
    }

    core::Path root_;
};

}  // namespace

core::Result<hal::Platform> createPlatform(const Esp32P4Config& config) {
    hal::Platform platform;
    platform.clock = std::make_unique<EspClock>();
    platform.storage = std::make_unique<NoCardStorage>(config.projectsRoot);
    platform.display = std::make_unique<SerialPreviewDisplay>(
        config.previewIntervalMillis,
        config.oled.sck >= 0 ? std::make_unique<EspSpiOledBus>(config.oled) : nullptr);
    platform.buttons = std::make_unique<EspGpioButtons>(config.pins);
    platform.encoder = std::make_unique<EspGpioEncoder>(config.pins);
    platform.audioOutput = std::make_unique<NullAudioOutput>();
    platform.audioInput = nullptr;
    EspUsbBackends usb = createUsbBackends();
    platform.usb = std::move(usb.usb);
    platform.mtp = std::move(usb.mtp);
    platform.deviceMode = std::move(usb.deviceMode);

    if (!platform.isComplete()) {
        return core::makeError(core::ErrorCode::Unknown, "esp32p4 platform incomplete");
    }
    core::log::info(kModule, "platform ready: SPI OLED (mirrored to serial), GPIO encoder "
                             "and buttons, USB host with TP-7 MTP; null storage and audio");
    return platform;
}

}  // namespace aw::platform::esp32p4
