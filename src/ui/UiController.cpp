#include "aw/ui/UiController.hpp"

#include "aw/ui/screens/ExportScreen.hpp"
#include "aw/ui/screens/HomeScreen.hpp"
#include "aw/ui/screens/ImportScreen.hpp"
#include "aw/ui/screens/PlaybackScreen.hpp"
#include "aw/ui/screens/ProjectBrowserScreen.hpp"
#include "aw/ui/screens/SettingsScreen.hpp"
#include "aw/ui/screens/TrackViewScreen.hpp"

namespace aw::ui {

UiController::UiController(display::Framebuffer& framebuffer,
                           display::DisplayFlusher& flusher,
                           events::EventBus& bus,
                           project::ProjectManager& projects,
                           audio::AudioEngine& engine)
    : framebuffer_{framebuffer},
      flusher_{flusher},
      bus_{bus},
      projects_{projects},
      engine_{engine},
      painter_{framebuffer} {}

UiController::~UiController() {
    if (subscription_ != events::kInvalidSubscription) {
        bus_.unsubscribe(subscription_);
    }
}

core::Status UiController::initialise() {
    navigator_.registerScreen(std::make_unique<HomeScreen>(projects_, engine_));
    navigator_.registerScreen(std::make_unique<ProjectBrowserScreen>(projects_));
    navigator_.registerScreen(std::make_unique<TrackViewScreen>(projects_, engine_));
    navigator_.registerScreen(std::make_unique<PlaybackScreen>(projects_, engine_));
    navigator_.registerScreen(std::make_unique<SettingsScreen>());

    // Import and Export need MTP collaborators; Phase 8 registers them from the
    // composition root, which is where those objects live.

    navigator_.reset(ScreenId::Home);

    subscription_ = bus_.subscribe([this](const events::Event& event) { onEvent(event); });
    return core::success();
}

void UiController::onEvent(const events::Event& event) {
    Screen* screen = navigator_.current();
    if (screen == nullptr) {
        return;
    }

    const NavigationRequest request = screen->handleEvent(event);
    if (request.intent == NavigationIntent::Ignored) {
        (void)handleGlobalShortcut(event);
        return;
    }
    navigator_.apply(request);
}

bool UiController::handleGlobalShortcut(const events::Event& event) {
    const auto* button = std::get_if<events::ButtonEvent>(&event);
    if (button == nullptr || button->action != core::ButtonAction::Pressed) {
        return false;
    }

    switch (button->id) {
        case core::ButtonId::Play:
            engine_.transport().requestPlay();
            return true;
        case core::ButtonId::Stop:
            engine_.transport().requestStop();
            return true;
        case core::ButtonId::Back:
            navigator_.pop();
            return true;
        case core::ButtonId::Undo:
            (void)projects_.undo();
            return true;
        case core::ButtonId::Save:
            (void)projects_.saveProject();
            return true;
        case core::ButtonId::Import:
            navigator_.push(ScreenId::Import);
            return true;
        case core::ButtonId::Export:
            navigator_.push(ScreenId::Export);
            return true;
        default:
            return false;
    }
}

core::Status UiController::tick(std::uint32_t elapsedMillis) {
    Screen* screen = navigator_.current();
    if (screen == nullptr) {
        return core::makeError(core::ErrorCode::InvalidState, "no active screen");
    }

    screen->update(elapsedMillis);

    if (screen->isDirty()) {
        screen->render(painter_);
    }

    (void)framebuffer_;
    return flusher_.flush();
}

}  // namespace aw::ui
