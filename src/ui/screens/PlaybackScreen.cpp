#include "aw/ui/screens/PlaybackScreen.hpp"

#include "aw/display/Color.hpp"
#include "aw/display/Font.hpp"

#include <array>
#include <algorithm>
#include <cstdio>

namespace aw::ui {

PlaybackScreen::PlaybackScreen(project::ProjectManager& projects,
                               audio::AudioEngine& engine) noexcept
    : projects_{projects}, engine_{engine} {}

NavigationRequest PlaybackScreen::handleEvent(const events::Event& event) {
    if (const auto* encoder = std::get_if<events::EncoderEvent>(&event)) {
        // Scrub: one detent is one second, which is the right granularity for
        // finding a spot in a take without a jog wheel.
        const auto step = core::millisToFrames(1000, 48000) * encoder->delta;
        engine_.transport().requestSeek(engine_.transport().position() + step);
        dirty_.set();
        return NavigationRequest::none();
    }
    if (const auto* button = std::get_if<events::ButtonEvent>(&event);
        button && button->id == core::ButtonId::EncoderPush &&
        button->action == core::ButtonAction::Pressed) {
        if (engine_.transport().state() == events::TransportState::Playing)
            engine_.transport().requestStop();
        else
            engine_.transport().requestPlay();
        dirty_.set();
        return NavigationRequest::none();
    }
    (void)projects_;
    return NavigationRequest::ignored();
}

void PlaybackScreen::update(std::uint32_t /*elapsedMillis*/) {
    const core::FrameIndex position = engine_.transport().position();
    dirty_.update(position != lastDrawnPosition_);
}

void PlaybackScreen::render(display::Painter& painter) {
    painter.clear(display::palette::kBackground);
    const auto& small = display::smallFont();
    const auto& large = display::largeFont();
    painter.fillRect({0, 0, 128, 12}, display::palette::kSurface);
    painter.drawText(2, 0, "TRANSPORT", small, display::palette::kText);
    const char* state = engine_.transport().state() == events::TransportState::Playing
                            ? "PLAY" : "STOP";
    painter.drawTextIn({90, 0, 36, 12}, state, small, display::palette::kText,
                       display::TextAlign::Right);
    const auto millis = core::framesToMillis(engine_.transport().position(), 48000);
    char time[32];  // Fits any long long minutes; 12 truncated past 99 minutes.
    std::snprintf(time, sizeof(time), "%02lld:%02lld.%03lld",
                  static_cast<long long>(millis / 60000),
                  static_cast<long long>((millis / 1000) % 60),
                  static_cast<long long>(millis % 1000));
    painter.drawTextIn({0, 13, 128, 16}, time, large, display::palette::kText,
                       display::TextAlign::Center);
    std::array<std::pair<float, float>, 32> envelope{};
    for (std::size_t i = 0; i < envelope.size(); ++i) {
        const float height = 0.15F + static_cast<float>((i * 7U) % 13U) / 16.0F;
        envelope[i] = {-height, height};
    }
    painter.drawWaveform({2, 32, 124, 18}, envelope, display::palette::kWaveform);
    const core::FrameCount length = projects_.hasProject() ? projects_.project().timeline().lengthFrames() : 0;
    if (length > 0) {
        const auto x = static_cast<std::int16_t>(2 + std::clamp<core::FrameIndex>(
            engine_.transport().position(), 0, length) * 123 / length);
        painter.drawVLine(x, 31, 20, display::palette::kAccent);
    }
    painter.drawTextIn({0, 52, 128, 12}, "PUSH PLAY / STOP", small,
                       display::palette::kTextDim, display::TextAlign::Center);
    lastDrawnPosition_ = engine_.transport().position();
    dirty_.clear();
}

}  // namespace aw::ui
