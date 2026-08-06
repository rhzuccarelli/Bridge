#pragma once

namespace aw::ui {

/// "Does this need redrawing?", as a composable member.
///
/// Every screen needs it; a shared base class would give them one at the cost
/// of an inheritance relationship that exists only to share three lines.  A
/// member is composition, costs a byte, and can be handed to a sub-view.
class DirtyFlag {
public:
    void set() noexcept { dirty_ = true; }
    void clear() noexcept { dirty_ = false; }
    [[nodiscard]] bool isSet() const noexcept { return dirty_; }

    /// Sets the flag if `changed` is true; returns `changed` so call sites read
    /// as `if (dirty_.update(value != previous)) { ... }`.
    bool update(bool changed) noexcept {
        dirty_ = dirty_ || changed;
        return changed;
    }

private:
    bool dirty_{true};  ///< A freshly constructed screen has never been drawn.
};

}  // namespace aw::ui
