#pragma once

#include <memory>

#include "aw/audio/AudioEngine.hpp"
#include "aw/devices/ApogeeDuet.hpp"
#include "aw/display/DisplayFlusher.hpp"
#include "aw/display/Framebuffer.hpp"
#include "aw/display/Painter.hpp"
#include "aw/events/EventBus.hpp"
#include "aw/hal/DeviceModeInterface.hpp"
#include "aw/project/ProjectManager.hpp"
#include "aw/project/ImportService.hpp"
#include "aw/project/ExportService.hpp"
#include "aw/mtp/MtpManager.hpp"
#include "aw/mtp/MtpBrowser.hpp"
#include "aw/ui/Navigator.hpp"
#include "aw/ui/TaskRunner.hpp"

namespace aw::ui {

/// Drives the user interface.
///
/// Responsibilities, and nothing else:
///  - subscribe to the event bus and route events to the current screen,
///  - apply the global shortcuts that work on every screen (transport, Back,
///    Undo, Save), so no screen has to reimplement them,
///  - render the current screen when it is dirty and flush the damage.
///
/// It holds references to the project manager and the engine so that screens can
/// be constructed with exactly the model access each one needs, and no more.
/// The controller never owns model state itself.
class UiController {
public:
    UiController(display::Framebuffer& framebuffer,
                 display::DisplayFlusher& flusher,
                 events::EventBus& bus,
                 project::ProjectManager& projects,
                 audio::AudioEngine& engine,
                 mtp::MtpManager* mtpManager = nullptr,
                 mtp::MtpBrowser* mtpBrowser = nullptr,
                 project::ImportService* importer = nullptr,
                 project::ExportService* exporter = nullptr,
                 TaskRunner* tasks = nullptr,
                 events::EventQueue* workerQueue = nullptr,
                 hal::DeviceModeInterface* deviceMode = nullptr,
                 devices::ApogeeDuet* duet = nullptr);
    ~UiController();

    UiController(const UiController&) = delete;
    UiController& operator=(const UiController&) = delete;

    /// Builds and registers every screen, then shows Home.
    [[nodiscard]] core::Status initialise();

    /// One pass: update the current screen, render if dirty, flush.
    /// Called from the main loop after the bus has dispatched.
    [[nodiscard]] core::Status tick(std::uint32_t elapsedMillis);

    [[nodiscard]] Navigator& navigator() noexcept { return navigator_; }

private:
    void onEvent(const events::Event& event);

    /// Buttons that mean the same thing everywhere.  Applied only after the
    /// current screen has returned `Ignored`, so a screen can still take one
    /// over when it genuinely needs to.
    [[nodiscard]] bool handleGlobalShortcut(const events::Event& event);

    display::Framebuffer& framebuffer_;
    display::DisplayFlusher& flusher_;
    events::EventBus& bus_;
    project::ProjectManager& projects_;
    audio::AudioEngine& engine_;
    mtp::MtpManager* mtpManager_;
    mtp::MtpBrowser* mtpBrowser_;
    project::ImportService* importer_;
    project::ExportService* exporter_;
    TaskRunner* tasks_;
    events::EventQueue* workerQueue_;
    hal::DeviceModeInterface* deviceMode_;
    devices::ApogeeDuet* duet_;

    Navigator navigator_;
    display::Painter painter_;
    events::SubscriptionId subscription_{events::kInvalidSubscription};
    bool encoderPushWasLong_{false};
};

}  // namespace aw::ui
