#include "aw/ui/screens/PlaybackScreen.hpp"

#include "aw/display/Color.hpp"

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
    (void)projects_;
    return NavigationRequest::ignored();
}

void PlaybackScreen::update(std::uint32_t /*elapsedMillis*/) {
    const core::FrameIndex position = engine_.transport().position();
    dirty_.update(position != lastDrawnPosition_);
}

void PlaybackScreen::render(display::Painter& painter) {
    // Phase 7 draws timecode, waveform overview with playhead, and meters.
    painter.clear(display::palette::kBackground);
    lastDrawnPosition_ = engine_.transport().position();
    dirty_.clear();
}

}  // namespace aw::ui
