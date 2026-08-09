#include "aw/app/Application.hpp"

#include "aw/core/Logger.hpp"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace aw::app {
namespace {
constexpr std::string_view kModule = "app";

class ThreadTaskRunner final : public ui::TaskRunner {
public:
    ThreadTaskRunner() : thread_{[this] { run(); }} {}
    ~ThreadTaskRunner() override {
        {
            std::lock_guard lock{mutex_};
            stopping_ = true;
        }
        condition_.notify_one();
        if (thread_.joinable()) thread_.join();
    }

    core::Status submit(std::function<void()> task) override {
        if (!task) return core::makeError(core::ErrorCode::InvalidArgument, "empty I/O task");
        {
            std::lock_guard lock{mutex_};
            if (stopping_) return core::makeError(core::ErrorCode::InvalidState, "I/O worker stopping");
            tasks_.push_back(std::move(task));
        }
        condition_.notify_one();
        return core::success();
    }

private:
    void run() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock{mutex_};
                condition_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
                if (stopping_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::function<void()>> tasks_;
    bool stopping_{false};
    std::thread thread_;
};
}

Application::Application(hal::Platform platform) : platform_{std::move(platform)} {}

Application::~Application() { shutdown(); }

core::Status Application::initialise() {
    if (!platform_.isComplete()) {
        return core::makeError(core::ErrorCode::InvalidState, "incomplete platform");
    }

    // --- Event plumbing ------------------------------------------------------
    bus_.addSource(inputQueue_);
    bus_.addSource(engineQueue_);
    bus_.addSource(projectQueue_);
    bus_.addSource(usbQueue_);
    bus_.addSource(workerQueue_);

    inputSource_ = std::make_unique<events::InputEventSource>(inputQueue_);
    platform_.buttons->setListener(inputSource_.get());
    platform_.encoder->setListener(inputSource_.get());

    // --- Model and engine ----------------------------------------------------
    projects_ = std::make_unique<project::ProjectManager>(*platform_.storage, projectQueue_);
    engine_ = std::make_unique<audio::AudioEngine>(*platform_.audioOutput, *platform_.storage,
                                                   engineQueue_);

    if (platform_.mtp) {
        mtpManager_ = std::make_unique<mtp::MtpManager>(*platform_.usb, *platform_.mtp,
                                                        usbQueue_);
        mtpBrowser_ = std::make_unique<mtp::MtpBrowser>(*platform_.mtp);
        importService_ = std::make_unique<project::ImportService>(
            *platform_.storage, *platform_.mtp, *projects_, workerQueue_);
        exportService_ = std::make_unique<project::ExportService>(
            *platform_.storage, *platform_.mtp, *projects_, workerQueue_);
        taskRunner_ = std::make_unique<ThreadTaskRunner>();
    }

    // --- Display -------------------------------------------------------------
    if (auto status = platform_.display->open(); !status) {
        return status;
    }
    const hal::DisplayInfo displayInfo = platform_.display->info();
    framebuffer_ = std::make_unique<display::Framebuffer>(displayInfo.width, displayInfo.height);
    flusher_ = std::make_unique<display::DisplayFlusher>(*platform_.display, *framebuffer_);

    uiController_ = std::make_unique<ui::UiController>(
        *framebuffer_, *flusher_, bus_, *projects_, *engine_, mtpManager_.get(),
        mtpBrowser_.get(), importService_.get(), exportService_.get(), taskRunner_.get(),
        &workerQueue_);
    if (auto status = uiController_->initialise(); !status) {
        return status;
    }

    // --- Hardware ------------------------------------------------------------
    if (auto status = platform_.buttons->start(); !status) {
        core::log::warning(kModule, "buttons unavailable: " + status.error().describe());
    }
    if (auto status = platform_.encoder->start(); !status) {
        core::log::warning(kModule, "encoder unavailable: " + status.error().describe());
    }
    if (auto status = engine_->start(kAudioBufferFrames); !status) {
        // Audio is the one subsystem whose absence makes the device pointless.
        return status;
    }
    if (mtpManager_) {
        if (auto status = mtpManager_->start(); !status) {
            core::log::warning(kModule, "USB unavailable: " + status.error().describe());
        }
    }

    initialised_ = true;
    core::log::info(kModule, "initialised");
    return core::success();
}

core::Status Application::run() {
    if (!initialised_) {
        return core::makeError(core::ErrorCode::InvalidState, "run() before initialise()");
    }

    std::uint64_t previousMicros = platform_.clock->nowMicros();
    std::uint32_t audioRefreshMillis = 0;

    while (!stopRequested_.load(std::memory_order_acquire)) {
        const std::uint64_t nowMicros = platform_.clock->nowMicros();
        const auto elapsedMillis = static_cast<std::uint32_t>((nowMicros - previousMicros) / 1000);
        previousMicros = nowMicros;
        audioRefreshMillis += elapsedMillis;

        if (audioRefreshMillis >= 2000) {
            audioRefreshMillis = 0;
            if (auto status = engine_->refreshOutput(); !status)
                core::log::warning(kModule, "audio refresh failed: " + status.error().describe());
        }

        bus_.dispatchPending();

        if (auto status = uiController_->tick(elapsedMillis); !status) {
            core::log::error(kModule, "ui tick failed: " + status.error().describe());
        }

        // Sleep the remainder of the frame.  Fixed rate rather than a busy
        // loop: the UI has nothing to do between frames and the CPU has better
        // things to do than spin.
        const std::uint64_t frameMicros = kFrameIntervalMillis * 1000ULL;
        const std::uint64_t spentMicros = platform_.clock->nowMicros() - nowMicros;
        if (spentMicros < frameMicros) {
            platform_.clock->sleepMicros(frameMicros - spentMicros);
        }
    }

    return core::success();
}

void Application::requestStop() noexcept {
    stopRequested_.store(true, std::memory_order_release);
}

void Application::shutdown() noexcept {
    if (!initialised_) {
        return;
    }
    initialised_ = false;

    // Screens submit work that captures their services. Destroy the screens,
    // then drain/join the I/O worker before closing the MTP session.
    uiController_.reset();
    taskRunner_.reset();
    if (mtpManager_) {
        mtpManager_->stop();
    }
    if (engine_) {
        engine_->stop();
    }
    if (platform_.encoder) {
        platform_.encoder->stop();
    }
    if (platform_.buttons) {
        platform_.buttons->stop();
    }
    if (platform_.display) {
        platform_.display->close();
    }

    core::log::info(kModule, "shutdown complete");
}

}  // namespace aw::app
