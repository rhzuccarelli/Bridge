#include "aw/ui/screens/SettingsScreen.hpp"

#include "aw/display/Color.hpp"
#include "aw/display/Font.hpp"

#include <array>
#include <algorithm>

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
    painter.clear(display::palette::kBackground);
    const auto& font = display::smallFont();
    painter.fillRect({0, 0, 128, 10}, display::palette::kSurface);
    painter.drawText(2, 1, "SETTINGS", font, display::palette::kText);
    constexpr std::array<std::string_view, 4> rows{
        "AUDIO  TP-7", "RATE   48K", "MON    96K/6", "GPIO   READY"};
    selection_ = std::clamp(selection_, 0, static_cast<int>(rows.size() - 1));
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto y = static_cast<std::int16_t>(14 + index * 11);
        if (static_cast<int>(index) == selection_) painter.drawRect(
            {0, static_cast<std::int16_t>(y - 1), 128, 10}, display::palette::kAccent);
        painter.drawText(4, y, rows[index], font, display::palette::kText);
    }
    dirty_.clear();
}

}  // namespace aw::ui
