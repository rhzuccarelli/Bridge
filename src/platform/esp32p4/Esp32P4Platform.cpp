#include "aw/platform/esp32p4/Esp32P4Platform.hpp"

#include <unistd.h>

#include <memory>
#include <vector>

#include "aw/core/Logger.hpp"
#include "aw/core/Thread.hpp"
#include "aw/platform/CompositeButtons.hpp"
#include "aw/platform/CompositeEncoder.hpp"
#include "aw/platform/FallbackAudioOutput.hpp"
#include "aw/platform/NullAudioOutput.hpp"
#include "aw/platform/esp32p4/EspConsoleInput.hpp"
#include "aw/platform/esp32p4/EspGpioInput.hpp"
#include "aw/platform/esp32p4/EspSpiOledBus.hpp"
#include "aw/platform/esp32p4/EspUsb.hpp"
#include "aw/platform/esp32p4/FatFsStorage.hpp"
#include "aw/platform/esp32p4/SerialPreviewDisplay.hpp"
#include "esp_pthread.h"
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

/// Stands in when no SD card mounts. An empty volume: the projects root exists
/// and lists nothing; anything that
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

/// Compute threads go to core 1. The USB host tasks are pinned to core 0, and
/// the TP-7 audio task, the other user of core 1, is gone while the TP-7 is in
/// MTP mode for an export. An export's render left on core 0 kept it busy
/// with the read threads it wakes, starving IDLE0 into watchdog warnings.
void placeThread(core::ThreadRole role) {
    esp_pthread_cfg_t config = esp_pthread_get_default_config();
    if (role == core::ThreadRole::Compute) {
        config.pin_to_core = 1;
        config.thread_name = "compute";
    }
    if (esp_pthread_set_cfg(&config) != ESP_OK) {
        core::log::warning(kModule, "cannot place the next thread");
    }
}

}  // namespace

core::Result<hal::Platform> createPlatform(const Esp32P4Config& config) {
    hal::Platform platform;
    core::threads::installPlacer(&placeThread);
    platform.clock = std::make_unique<EspClock>();
    if (auto card = FatFsStorage::mount(config.sdCard, config.projectsRoot); card) {
        if (auto status = card.value()->createDirectories(config.projectsRoot); !status) {
            core::log::warning(kModule, "cannot create projects folder: " + status.error().describe());
        }
        platform.storage = std::move(card).value();
    } else {
        core::log::warning(kModule, card.error().describe() + "; running without storage");
        platform.storage = std::make_unique<NoCardStorage>(config.projectsRoot);
    }
    platform.display = std::make_unique<SerialPreviewDisplay>(
        config.previewIntervalMillis,
        config.oled.sck >= 0 ? std::make_unique<EspSpiOledBus>(config.oled) : nullptr);
    // The panel, plus keys typed on the serial console (development input).
    EspConsoleInput console;
    auto buttons = std::make_unique<CompositeButtons>();
    buttons->addSource(std::make_unique<EspGpioButtons>(config.pins));
    buttons->addSource(console.buttons());
    platform.buttons = std::move(buttons);
    auto encoder = std::make_unique<CompositeEncoder>();
    encoder->addSource(std::make_unique<EspGpioEncoder>(config.pins));
    encoder->addSource(console.encoder());
    platform.encoder = std::move(encoder);
    platform.audioInput = nullptr;
    EspUsbBackends usb = createUsbBackends();
    // Monitor audio through the TP-7 over USB. While it is unplugged or in MTP
    // mode, the silent wall-clock output keeps the transport running, and
    // refresh() switches back once the TP-7 returns to USB audio.
    platform.audioOutput = std::make_unique<FallbackAudioOutput>(std::move(usb.audioOutput),
                                                                 std::make_unique<NullAudioOutput>());
    platform.usb = std::move(usb.usb);
    platform.mtp = std::move(usb.mtp);
    platform.deviceMode = std::move(usb.deviceMode);

    if (!platform.isComplete()) {
        return core::makeError(core::ErrorCode::Unknown, "esp32p4 platform incomplete");
    }
    core::log::info(kModule, "platform ready: SPI OLED (mirrored to serial), GPIO encoder "
                             "and buttons, USB host with TP-7 MTP and monitor audio, SD card storage");
    return platform;
}

}  // namespace aw::platform::esp32p4
