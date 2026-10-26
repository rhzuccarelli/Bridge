#include "aw/display/Framebuffer.hpp"
#include "aw/display/Painter.hpp"
#include "aw/ui/Navigator.hpp"
#include "aw/ui/screens/DuetScreen.hpp"
#include "framework/Test.hpp"

#include <array>
#include <functional>
#include <span>

using namespace aw;

namespace {

/// A screen that records what happened to it.  Enough to test the navigator
/// without constructing the real screens (and therefore without a project
/// manager, an engine, or any hardware) - which is the point of the
/// `Screen` interface being this small.
class SpyScreen final : public ui::Screen {
public:
    explicit SpyScreen(ui::ScreenId id) : id_{id} {}

    [[nodiscard]] ui::ScreenId id() const noexcept override { return id_; }
    [[nodiscard]] std::string_view title() const noexcept override { return "Spy"; }

    void onEnter() override { ++enterCount; }
    void onExit() override { ++exitCount; }

    [[nodiscard]] ui::NavigationRequest handleEvent(const events::Event&) override {
        return ui::NavigationRequest::none();
    }
    [[nodiscard]] bool isDirty() const noexcept override { return false; }
    void render(display::Painter&) override {}

    int enterCount{0};
    int exitCount{0};

private:
    ui::ScreenId id_;
};

}  // namespace

AW_TEST(Navigator, ResetShowsTheGivenScreen) {
    ui::Navigator navigator;
    auto home = std::make_unique<SpyScreen>(ui::ScreenId::Home);
    SpyScreen* homePtr = home.get();
    navigator.registerScreen(std::move(home));

    navigator.reset(ui::ScreenId::Home);

    AW_CHECK_EQ(navigator.depth(), std::size_t{1});
    AW_CHECK(navigator.current() == homePtr);
    AW_CHECK_EQ(homePtr->enterCount, 1);
}

AW_TEST(Navigator, PushAndPopCallEnterAndExit) {
    ui::Navigator navigator;
    auto home = std::make_unique<SpyScreen>(ui::ScreenId::Home);
    auto settings = std::make_unique<SpyScreen>(ui::ScreenId::Settings);
    SpyScreen* homePtr = home.get();
    SpyScreen* settingsPtr = settings.get();
    navigator.registerScreen(std::move(home));
    navigator.registerScreen(std::move(settings));

    navigator.reset(ui::ScreenId::Home);
    navigator.push(ui::ScreenId::Settings);

    AW_CHECK_EQ(navigator.depth(), std::size_t{2});
    AW_CHECK(navigator.current() == settingsPtr);
    AW_CHECK_EQ(homePtr->exitCount, 1);
    AW_CHECK_EQ(settingsPtr->enterCount, 1);

    navigator.pop();
    AW_CHECK(navigator.current() == homePtr);
    AW_CHECK_EQ(settingsPtr->exitCount, 1);
    AW_CHECK_EQ(homePtr->enterCount, 2);
}

AW_TEST(Navigator, PoppingTheRootIsANoOp) {
    ui::Navigator navigator;
    navigator.registerScreen(std::make_unique<SpyScreen>(ui::ScreenId::Home));
    navigator.reset(ui::ScreenId::Home);

    navigator.pop();
    AW_CHECK_EQ(navigator.depth(), std::size_t{1});
}

AW_TEST(Navigator, ReplaceKeepsTheDepth) {
    ui::Navigator navigator;
    navigator.registerScreen(std::make_unique<SpyScreen>(ui::ScreenId::Home));
    navigator.registerScreen(std::make_unique<SpyScreen>(ui::ScreenId::TrackView));

    navigator.reset(ui::ScreenId::Home);
    navigator.replace(ui::ScreenId::TrackView);

    AW_CHECK_EQ(navigator.depth(), std::size_t{1});
    AW_REQUIRE(navigator.current() != nullptr);
    AW_CHECK_EQ(navigator.current()->id(), ui::ScreenId::TrackView);
}

AW_TEST(Navigator, AppliesScreenRequests) {
    ui::Navigator navigator;
    navigator.registerScreen(std::make_unique<SpyScreen>(ui::ScreenId::Home));
    navigator.registerScreen(std::make_unique<SpyScreen>(ui::ScreenId::Import));
    navigator.reset(ui::ScreenId::Home);

    AW_CHECK(!navigator.apply(ui::NavigationRequest::none()));
    AW_CHECK(!navigator.apply(ui::NavigationRequest::ignored()));

    AW_CHECK(navigator.apply(ui::NavigationRequest::push(ui::ScreenId::Import)));
    AW_CHECK_EQ(navigator.depth(), std::size_t{2});

    AW_CHECK(navigator.apply(ui::NavigationRequest::back()));
    AW_CHECK_EQ(navigator.depth(), std::size_t{1});
}

namespace {

/// Runs each task at once, on the caller's thread.
class InlineTaskRunner final : public ui::TaskRunner {
public:
    core::Status submit(std::function<void()> task) override {
        task();
        return core::success();
    }
};

/// Input types behind endpoint 0, as an Apogee Duet keeps them.
class FakeDuetUsb final : public hal::UsbControlInterface {
public:
    core::Result<std::size_t> control(std::uint16_t, std::uint16_t, const hal::UsbControlSetup& setup,
                                      std::span<std::uint8_t> data) override {
        if (!attached) return core::makeError(core::ErrorCode::DeviceNotConnected, "no Duet");
        if (setup.request != 22) return data.size();  // Grouping: accepted, not modelled.
        if ((setup.requestType & 0x80) != 0) data[0] = types[setup.index];
        else {
            types[setup.index] = data[0];
            ++writes;
        }
        return data.size();
    }
    bool attached{true};
    std::array<std::uint8_t, 2> types{0, 0};
    int writes{0};
};

/// Hands every event the worker posted to the screen.
void deliver(events::EventQueue& queue, ui::Screen& screen) {
    while (auto event = queue.poll()) (void)screen.handleEvent(*event);
}

events::Event press(core::ButtonId id) {
    return events::ButtonEvent{id, core::ButtonAction::Pressed, 0};
}

}  // namespace

AW_TEST(DuetScreen, SendsOnlyTheConfirmedType) {
    FakeDuetUsb usb;
    devices::ApogeeDuet duet{usb};
    InlineTaskRunner tasks;
    events::EventQueue queue;
    ui::DuetScreen screen{duet, tasks, queue};
    screen.onEnter();
    deliver(queue, screen);

    (void)screen.handleEvent(press(core::ButtonId::EncoderPush));  // Edit input 1.
    (void)screen.handleEvent(events::EncoderEvent{3, 0});          // +4dBu -> Instrument.
    AW_CHECK_EQ(usb.writes, 0);
    (void)screen.handleEvent(press(core::ButtonId::EncoderPush));
    deliver(queue, screen);

    AW_CHECK_EQ(usb.writes, 1);
    AW_CHECK_EQ(usb.types[0], std::uint8_t{3});
    AW_CHECK_EQ(usb.types[1], std::uint8_t{0});
}

AW_TEST(DuetScreen, BackCancelsAnEditWithoutLeaving) {
    FakeDuetUsb usb;
    devices::ApogeeDuet duet{usb};
    InlineTaskRunner tasks;
    events::EventQueue queue;
    ui::DuetScreen screen{duet, tasks, queue};
    screen.onEnter();
    deliver(queue, screen);

    (void)screen.handleEvent(events::EncoderEvent{1, 0});  // Input 2.
    (void)screen.handleEvent(press(core::ButtonId::EncoderPush));
    (void)screen.handleEvent(events::EncoderEvent{2, 0});
    const auto back = screen.handleEvent(press(core::ButtonId::Back));
    AW_CHECK(back.intent != ui::NavigationIntent::Ignored);  // Not a pop.
    AW_CHECK_EQ(usb.writes, 0);
    // Out of the edit, Back leaves the screen as everywhere else.
    AW_CHECK(screen.handleEvent(press(core::ButtonId::Back)).intent == ui::NavigationIntent::Ignored);
}

AW_TEST(DuetScreen, RendersWithoutADuet) {
    FakeDuetUsb usb;
    usb.attached = false;
    devices::ApogeeDuet duet{usb};
    InlineTaskRunner tasks;
    events::EventQueue queue;
    ui::DuetScreen screen{duet, tasks, queue};
    screen.onEnter();
    deliver(queue, screen);
    (void)screen.handleEvent(press(core::ButtonId::EncoderPush));  // Retries; still none.
    deliver(queue, screen);
    AW_CHECK_EQ(usb.writes, 0);

    display::Framebuffer framebuffer{128, 64};
    display::Painter painter{framebuffer};
    screen.render(painter);
    AW_CHECK(!screen.isDirty());
}
