#include "aw/platform/simulator/SimulatorPlatform.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "aw/core/Logger.hpp"
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

/// Wall-clock audio output.
///
/// Calls the render callback on a real schedule and discards the audio.  That
/// is enough for transport, streaming and meter logic to be exercised end to
/// end without a sound card - and it will surface a callback that takes too
/// long just like real hardware would.
class TimedNullAudioOutput final : public hal::AudioOutputInterface {
public:
    ~TimedNullAudioOutput() override { close(); }

    core::Status open(const core::AudioFormat& format,
                      core::FrameCount bufferFrames,
                      hal::AudioRenderCallback& callback) override {
        format_ = format;
        bufferFrames_ = bufferFrames;
        callback_ = &callback;
        scratch_.assign(static_cast<std::size_t>(bufferFrames) * format.channels, 0.0F);
        return core::success();
    }

    core::Status start() override {
        if (callback_ == nullptr) {
            return core::makeError(core::ErrorCode::InvalidState, "audio output not open");
        }
        if (running_.exchange(true)) {
            return core::success();
        }
        thread_ = std::thread{[this] { run(); }};
        return core::success();
    }

    void stop() noexcept override {
        if (running_.exchange(false) && thread_.joinable()) {
            thread_.join();
        }
    }

    void close() noexcept override {
        stop();
        callback_ = nullptr;
        scratch_.clear();
    }

    bool isRunning() const noexcept override { return running_.load(); }
    core::AudioFormat format() const noexcept override { return format_; }
    core::FrameCount bufferFrames() const noexcept override { return bufferFrames_; }
    core::FrameCount latencyFrames() const noexcept override { return bufferFrames_; }

private:
    void run() {
        const auto period = std::chrono::microseconds{
            (static_cast<std::int64_t>(bufferFrames_) * 1'000'000) /
            static_cast<std::int64_t>(format_.sampleRate)};
        auto next = std::chrono::steady_clock::now();

        while (running_.load()) {
            next += period;
            callback_->renderAudio(scratch_, bufferFrames_, format_.channels);
            std::this_thread::sleep_until(next);
        }
    }

    core::AudioFormat format_{};
    core::FrameCount bufferFrames_{0};
    hal::AudioRenderCallback* callback_{nullptr};
    std::vector<float> scratch_;
    std::atomic<bool> running_{false};
    std::thread thread_;
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
    platform.audioOutput = std::make_unique<TimedNullAudioOutput>();
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
