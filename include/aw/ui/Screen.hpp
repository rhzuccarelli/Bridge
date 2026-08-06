#pragma once

#include <cstdint>
#include <string_view>

#include "aw/display/Painter.hpp"
#include "aw/events/Event.hpp"

namespace aw::ui {

/// Every screen in the device.
///
/// Closed enum rather than open registration: the navigation graph is a product
/// decision (see docs/architecture.md), and being able to see every destination
/// in one list is worth more than extensibility on a device whose screens ship
/// in firmware.
enum class ScreenId : std::uint8_t {
    Home,
    ProjectBrowser,
    TrackView,
    Playback,
    Import,
    Export,
    Settings,
};

[[nodiscard]] std::string_view toString(ScreenId id) noexcept;

/// What a screen wants to happen next.  Returned from `handleEvent` so screens
/// never call the navigator directly - they describe intent, the navigator
/// decides.  That keeps screens independently testable: drive an event in,
/// assert on the returned intent, no navigator required.
enum class NavigationIntent : std::uint8_t {
    None,      ///< Handled, stay here.
    Ignored,   ///< Not handled; let the global shortcut layer see it.
    Back,      ///< Pop.
    Push,      ///< Push `requestedScreen`.
    Replace,   ///< Replace the current screen with `requestedScreen`.
};

struct NavigationRequest {
    NavigationIntent intent{NavigationIntent::None};
    ScreenId requestedScreen{ScreenId::Home};

    static NavigationRequest none() noexcept { return {}; }
    static NavigationRequest ignored() noexcept {
        return NavigationRequest{NavigationIntent::Ignored, ScreenId::Home};
    }
    static NavigationRequest back() noexcept {
        return NavigationRequest{NavigationIntent::Back, ScreenId::Home};
    }
    static NavigationRequest push(ScreenId screen) noexcept {
        return NavigationRequest{NavigationIntent::Push, screen};
    }
    static NavigationRequest replace(ScreenId screen) noexcept {
        return NavigationRequest{NavigationIntent::Replace, screen};
    }
};

/// A full-screen view.
///
/// Immediate mode: `render` draws the whole screen from the model every time it
/// is called, and `isDirty` decides whether it is called at all.  There are no
/// widgets holding state, so there is nothing to get out of sync with the
/// project.
class Screen {
public:
    virtual ~Screen() = default;

    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;

    [[nodiscard]] virtual ScreenId id() const noexcept = 0;

    /// Shown in the title bar.
    [[nodiscard]] virtual std::string_view title() const noexcept = 0;

    virtual void onEnter() {}
    virtual void onExit() {}

    /// Handles one event.  Return `Ignored` for anything this screen does not
    /// own, so global shortcuts (transport buttons work everywhere) still fire.
    [[nodiscard]] virtual NavigationRequest handleEvent(const events::Event& event) = 0;

    /// Called once per frame with elapsed milliseconds, for anything animated.
    virtual void update(std::uint32_t /*elapsedMillis*/) {}

    /// True when the screen's appearance has changed since the last render.
    /// Screens that show a moving playhead simply return true.
    [[nodiscard]] virtual bool isDirty() const noexcept = 0;

    virtual void render(display::Painter& painter) = 0;

protected:
    Screen() = default;
};

}  // namespace aw::ui
