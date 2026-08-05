#include "aw/ui/Navigator.hpp"
#include "framework/Test.hpp"

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
