#include "aw/ui/UiController.hpp"

#include "aw/ui/screens/ExportScreen.hpp"
#include "aw/ui/screens/CopyChannelScreen.hpp"
#include "aw/ui/screens/DeckScreen.hpp"
#include "aw/ui/screens/HomeScreen.hpp"
#include "aw/ui/screens/ImportScreen.hpp"
#include "aw/ui/screens/PlaybackScreen.hpp"
#include "aw/ui/screens/ProjectBrowserScreen.hpp"
#include "aw/ui/screens/SettingsScreen.hpp"
#include "aw/ui/screens/TrackViewScreen.hpp"

#include "aw/display/Color.hpp"
#include "aw/display/Font.hpp"

namespace {

class TransferPlaceholder final : public aw::ui::Screen {
public:
    TransferPlaceholder(aw::ui::ScreenId id, std::string_view title) : id_{id}, title_{title} {}
    aw::ui::ScreenId id() const noexcept override { return id_; }
    std::string_view title() const noexcept override { return title_; }
    void onEnter() override { dirty_ = true; }
    aw::ui::NavigationRequest handleEvent(const aw::events::Event&) override {
        return aw::ui::NavigationRequest::ignored();
    }
    bool isDirty() const noexcept override { return dirty_; }
    void render(aw::display::Painter& painter) override {
        painter.clear(aw::display::palette::kBackground);
        painter.fillRect({0, 0, 128, 10}, aw::display::palette::kSurface);
        painter.drawText(2, 1, title_, aw::display::smallFont(), aw::display::palette::kText);
        painter.drawTextIn({0, 20, 128, 10}, "MTP UNAVAILABLE", aw::display::smallFont(),
                           aw::display::palette::kTextDim, aw::display::TextAlign::Center);
        dirty_ = false;
    }
private:
    aw::ui::ScreenId id_;
    std::string_view title_;
    bool dirty_{true};
};

}  // namespace

namespace aw::ui {

UiController::UiController(display::Framebuffer& framebuffer,
                           display::DisplayFlusher& flusher,
                           events::EventBus& bus,
                           project::ProjectManager& projects,
                           audio::AudioEngine& engine,
                           mtp::MtpManager* mtpManager,
                           mtp::MtpBrowser* mtpBrowser,
                           project::ImportService* importer,
                           project::ExportService* exporter,
                           TaskRunner* tasks,
                           events::EventQueue* workerQueue,
                           hal::DeviceModeInterface* deviceMode)
    : framebuffer_{framebuffer},
      flusher_{flusher},
      bus_{bus},
      projects_{projects},
      engine_{engine},
      mtpManager_{mtpManager},
      mtpBrowser_{mtpBrowser},
      importer_{importer},
      exporter_{exporter},
      tasks_{tasks},
      workerQueue_{workerQueue},
      deviceMode_{deviceMode},
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
    navigator_.registerScreen(std::make_unique<CopyChannelScreen>(projects_));
    navigator_.registerScreen(std::make_unique<DeckScreen>(projects_));
    navigator_.registerScreen(std::make_unique<PlaybackScreen>(projects_, engine_));
    navigator_.registerScreen(std::make_unique<SettingsScreen>());
    if (mtpManager_ && mtpBrowser_ && importer_ && exporter_ && tasks_ && workerQueue_) {
        navigator_.registerScreen(std::make_unique<ImportScreen>(
            *mtpManager_, *mtpBrowser_, *importer_, *tasks_, *workerQueue_, deviceMode_));
        navigator_.registerScreen(std::make_unique<ExportScreen>(
            *mtpManager_, *mtpBrowser_, projects_, *exporter_, *tasks_, *workerQueue_));
    } else {
        navigator_.registerScreen(std::make_unique<TransferPlaceholder>(ScreenId::Import, "IMPORT"));
        navigator_.registerScreen(std::make_unique<TransferPlaceholder>(ScreenId::Export, "EXPORT"));
    }

    navigator_.reset(ScreenId::Home);

    subscription_ = bus_.subscribe([this](const events::Event& event) { onEvent(event); });
    return core::success();
}

void UiController::onEvent(const events::Event& event) {
    events::Event shortPressEvent = event;
    const events::Event* routedEvent = &event;
    if (const auto* button = std::get_if<events::ButtonEvent>(&event);
        button && button->id == core::ButtonId::EncoderPush) {
        if (button->action == core::ButtonAction::Pressed) {
            encoderPushWasLong_ = false;
            return;  // Defer activation until release, after the hold threshold.
        }
        if (button->action == core::ButtonAction::LongPressed) {
            encoderPushWasLong_ = true;
            navigator_.pop();
            return;
        }
        if (button->action == core::ButtonAction::Repeat) return;
        if (button->action == core::ButtonAction::Released) {
            if (encoderPushWasLong_) return;
            shortPressEvent = events::ButtonEvent{button->id, core::ButtonAction::Pressed,
                                                   button->timestampMicros};
            routedEvent = &shortPressEvent;
        }
    }

    if (const auto* projectEvent = std::get_if<events::ProjectEvent>(routedEvent)) {
        if (projectEvent->change == events::ProjectChange::Opened ||
            projectEvent->change == events::ProjectChange::Modified ||
            projectEvent->change == events::ProjectChange::UndoStackChanged) {
            (void)engine_.setScene(projects_.buildScene());
        }
    }
    Screen* screen = navigator_.current();
    if (screen == nullptr) {
        return;
    }

    const NavigationRequest request = screen->handleEvent(*routedEvent);
    if (request.intent == NavigationIntent::Ignored) {
        (void)handleGlobalShortcut(*routedEvent);
        return;
    }
    navigator_.apply(request);
}

bool UiController::handleGlobalShortcut(const events::Event& event) {
    const auto* button = std::get_if<events::ButtonEvent>(&event);
    if (button == nullptr) {
        return false;
    }
    if (button->action != core::ButtonAction::Pressed) return false;

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
