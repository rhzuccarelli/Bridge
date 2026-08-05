#include "aw/ui/screens/SettingsScreen.hpp"

#include "aw/display/Color.hpp"

namespace aw::ui {

NavigationRequest SettingsScreen::handleEvent(const events::Event& event) {
    if (const auto* encoder = std::get_if<events::EncoderEvent>(&event)) {
        selection_ += encoder->delta;
        selection_ = selection_ < 0 ? 0 : selection_;
        dirty_.set();
        return NavigationRequest::none();
    }
    return NavigationRequest::ignored();
}

void SettingsScreen::render(display::Painter& painter) {
    // Phase 7: buffer size, backlight, diagnostics.
    painter.clear(display::palette::kBackground);
    dirty_.clear();
}

}  // namespace aw::ui
