#pragma once

#include <atomic>
#include <memory>

#include "aw/audio/AudioEngine.hpp"
#include "aw/core/Result.hpp"
#include "aw/display/DisplayFlusher.hpp"
#include "aw/display/Framebuffer.hpp"
#include "aw/events/EventBus.hpp"
#include "aw/events/EventQueue.hpp"
#include "aw/events/InputEventSource.hpp"
#include "aw/hal/Platform.hpp"
#include "aw/mtp/MtpBrowser.hpp"
#include "aw/mtp/MtpManager.hpp"
#include "aw/project/ExportService.hpp"
#include "aw/project/ImportService.hpp"
#include "aw/project/ProjectManager.hpp"
#include "aw/ui/UiController.hpp"

namespace aw::app {

/// Wires the whole device together and runs the main loop.
///
/// This is the composition root: the one class that knows every other class
/// exists.  Everything it builds receives its collaborators by reference, so no
/// module has to find anything for itself - which is exactly why there are no
/// singletons or service locators anywhere else.
///
/// The main loop is a fixed-rate tick, not an idle spin:
///   1. dispatch pending events (input, engine, I/O worker),
///   2. tick the UI (update, render if dirty, flush damage),
///   3. sleep to the next frame.
/// At 30 Hz that is 33 ms of budget for a redraw that costs well under one, so
/// the loop is almost always asleep - which is what keeps a passively cooled
/// board cool and, later, an ESP32 within its power budget.
class Application {
public:
    /// Takes ownership of the platform built by the composition root.
    explicit Application(hal::Platform platform);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /// Constructs every subsystem and starts audio, input and USB.
    [[nodiscard]] core::Status initialise();

    /// Runs until `requestStop()`.  Returns the first fatal error, if any.
    [[nodiscard]] core::Status run();

    /// Safe to call from a signal handler or another thread.
    void requestStop() noexcept;

    void shutdown() noexcept;

private:
    static constexpr std::uint32_t kFrameIntervalMillis = 33;  // ~30 Hz.
    static constexpr core::FrameCount kAudioBufferFrames = 256;

    hal::Platform platform_;

    events::EventQueue inputQueue_;   ///< Produced by the input backends.
    events::EventQueue engineQueue_;  ///< Produced by the audio engine.
    events::EventQueue workerQueue_;  ///< Produced by the I/O worker.
    events::EventBus bus_;
    std::unique_ptr<events::InputEventSource> inputSource_;

    std::unique_ptr<audio::AudioEngine> engine_;
    std::unique_ptr<project::ProjectManager> projects_;
    std::unique_ptr<mtp::MtpManager> mtpManager_;
    std::unique_ptr<mtp::MtpBrowser> mtpBrowser_;
    std::unique_ptr<project::ImportService> importService_;
    std::unique_ptr<project::ExportService> exportService_;

    std::unique_ptr<display::Framebuffer> framebuffer_;
    std::unique_ptr<display::DisplayFlusher> flusher_;
    std::unique_ptr<ui::UiController> uiController_;

    std::atomic<bool> stopRequested_{false};
    bool initialised_{false};
};

}  // namespace aw::app
