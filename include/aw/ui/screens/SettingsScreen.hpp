#pragma once

#include "aw/ui/DirtyFlag.hpp"
#include "aw/ui/Screen.hpp"

namespace aw::ui {

/// Device settings and diagnostics.
///
/// Kept short on purpose: buffer size, backlight, and a diagnostics page showing
/// xruns, dropped events and free card space.  Everything else that would
/// normally be a setting is a fixed decision elsewhere (48 kHz, 24 bit, four
/// tracks), because options a hardware recorder does not need are options the
/// user has to scroll past.
class SettingsScreen final : public Screen {
public:
    SettingsScreen() noexcept = default;

    [[nodiscard]] ScreenId id() const noexcept override { return ScreenId::Settings; }
    [[nodiscard]] std::string_view title() const noexcept override { return "Settings"; }

    [[nodiscard]] NavigationRequest handleEvent(const events::Event& event) override;
    [[nodiscard]] bool isDirty() const noexcept override { return dirty_.isSet(); }
    void render(display::Painter& painter) override;

private:
    DirtyFlag dirty_;
    int selection_{0};
};

}  // namespace aw::ui
