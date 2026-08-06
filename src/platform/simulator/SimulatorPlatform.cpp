#include "aw/platform/simulator/SimulatorPlatform.hpp"

#include <chrono>
#include <thread>
#include <vector>

#include "aw/core/Logger.hpp"
#include "aw/platform/NullAudioOutput.hpp"
#include "aw/platform/desktop/DevBackends.hpp"
#include "aw/platform/posix/PosixStorage.hpp"

namespace aw::platform::simulator {
namespace {

constexpr std::string_view kModule = "simulator";

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

class NullUsb final : public hal::USBInterface {
public:
    core::Status start() override { return core::success(); }
    void stop() noexcept override {}
    void setHotplugListener(hal::UsbHotplugListener* listener) noexcept override {
        listener_ = listener;
    }
    core::Result<std::vector<hal::UsbDeviceInfo>> enumerate() const override {
        return std::vector<hal::UsbDeviceInfo>{};
    }

private:
    hal::UsbHotplugListener* listener_{nullptr};
};

/// Reports "nothing plugged in" for every call, which is exactly what the UI
/// must handle gracefully anyway.
class NullMtp final : public hal::MTPInterface {
public:
    core::Status openSession(std::uint32_t /*deviceKey*/) override { return notConnected(); }
    void closeSession() noexcept override {}
    bool hasSession() const noexcept override { return false; }

    core::Result<hal::MtpDeviceInfo> deviceInfo() const override { return notConnected(); }
    core::Result<std::vector<hal::MtpStorageInfo>> storages() const override {
        return notConnected();
    }
    core::Result<std::vector<hal::MtpObjectInfo>> listObjects(
        hal::MtpStorageId /*storage*/, hal::MtpObjectHandle /*parent*/) override {
        return notConnected();
    }
    core::Result<hal::MtpObjectInfo> objectInfo(hal::MtpObjectHandle /*handle*/) override {
        return notConnected();
    }
    core::Status readObject(hal::MtpObjectHandle /*handle*/,
                            const hal::MtpWriteSink& /*sink*/,
                            const hal::MtpProgressCallback& /*progress*/) override {
        return notConnected();
    }
    core::Result<hal::MtpObjectHandle> writeObject(
        hal::MtpStorageId /*storage*/,
        hal::MtpObjectHandle /*parent*/,
        const std::string& /*name*/,
        std::uint64_t /*sizeBytes*/,
        const hal::MtpReadSource& /*source*/,
        const hal::MtpProgressCallback& /*progress*/) override {
        return notConnected();
    }
    core::Status deleteObject(hal::MtpObjectHandle /*handle*/) override { return notConnected(); }

private:
    static core::Error notConnected() {
        return core::makeError(core::ErrorCode::DeviceNotConnected, "simulator has no MTP device");
    }
};

}  // namespace

core::Result<hal::Platform> createPlatform(const core::Path& projectsRoot) {
    hal::Platform platform;
    platform.clock = std::make_unique<SteadyClock>();
    platform.storage = std::make_unique<posix::PosixStorage>(projectsRoot);
    platform.audioOutput = std::make_unique<NullAudioOutput>();
    platform.audioInput = nullptr;
    platform.usb = std::make_unique<NullUsb>();
    platform.mtp = std::make_unique<NullMtp>();

    // Display and input come from whatever is actually attached: the Linux
    // framebuffer with a touch key strip on the bench, an ANSI terminal preview
    // over SSH, or nothing at all in CI.  Selection lives in one place so both
    // this and the Pi assembly behave identically.
    desktop::DevBackends backends = desktop::createDevBackends(desktop::optionsFromEnvironment());
    platform.display = std::move(backends.display);
    platform.buttons = std::move(backends.buttons);
    platform.encoder = std::move(backends.encoder);

    if (!platform.isComplete()) {
        return core::makeError(core::ErrorCode::Unknown, "simulator platform incomplete");
    }

    core::log::info(kModule, "development platform ready: " + backends.description +
                                 " (projects: " + projectsRoot.string() + ")");
    return platform;
}

}  // namespace aw::platform::simulator
