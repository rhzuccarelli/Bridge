#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "aw/ui/Screen.hpp"

namespace aw::ui {

/// Owns the screens and the navigation stack.
///
/// A stack, but a shallow one: the interface brief says "avoid nested menus",
/// and the stack exists for the Back button rather than for depth.  In practice
/// nothing goes deeper than Home -> destination -> confirmation.  `kMaxDepth`
/// makes that a checked invariant instead of a hope - exceeding it is a design
/// regression, and it will fail loudly in tests.
class Navigator {
public:
    static constexpr std::size_t kMaxDepth = 4;

    Navigator() = default;

    Navigator(const Navigator&) = delete;
    Navigator& operator=(const Navigator&) = delete;

    /// Registers a screen.  Called once at startup; the navigator owns them all
    /// for the lifetime of the app, because allocating a screen when the user
    /// presses a button is exactly the kind of latency this device must not have.
    void registerScreen(std::unique_ptr<Screen> screen);

    /// Makes `id` the only screen on the stack.
    void reset(ScreenId id);

    [[nodiscard]] Screen* current() noexcept;
    [[nodiscard]] const Screen* current() const noexcept;

    void push(ScreenId id);
    void pop();
    void replace(ScreenId id);

    /// Applies a screen's returned request.  Returns true if it navigated.
    bool apply(const NavigationRequest& request);

    [[nodiscard]] std::size_t depth() const noexcept { return stack_.size(); }

private:
    std::unordered_map<std::uint8_t, std::unique_ptr<Screen>> screens_;
    std::vector<ScreenId> stack_;
};

}  // namespace aw::ui
