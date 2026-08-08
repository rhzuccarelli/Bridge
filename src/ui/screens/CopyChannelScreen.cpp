#include "aw/ui/screens/CopyChannelScreen.hpp"

#include "aw/display/Color.hpp"
#include "aw/display/Font.hpp"
#include "aw/project/CopyChannelCommand.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>

namespace aw::ui {

void CopyChannelScreen::onEnter() {
    step_ = 0;
    sourceDeck_ = 0;
    destinationDeck_ = 0;
    error_ = core::ErrorCode::Unknown;
    if (projects_.hasProject()) {
        const auto& decks = projects_.project().decks();
        for (std::size_t index = 0; index < decks.size(); ++index) {
            if (decks[index].id == projects_.project().activeDeckId()) destinationDeck_ = index;
        }
    }
    dirty_.set();
}

void CopyChannelScreen::change(int delta) {
    if (!projects_.hasProject() || delta == 0) return;
    const std::size_t deckCount = projects_.project().decks().size();
    const auto wrap = [delta](std::size_t value, std::size_t count) {
        const int modulus = static_cast<int>(count);
        return static_cast<std::size_t>((static_cast<int>(value) + delta % modulus + modulus) % modulus);
    };
    switch (step_) {
        case 0: sourceDeck_ = wrap(sourceDeck_, deckCount); break;
        case 1: sourceTrack_ = wrap(sourceTrack_, project::kTrackCount); break;
        case 2: sourceSide_ = static_cast<core::ChannelCount>(wrap(sourceSide_, 2)); break;
        case 3: destinationDeck_ = wrap(destinationDeck_, deckCount); break;
        case 4: destinationTrack_ = wrap(destinationTrack_, project::kTrackCount); break;
        case 5: destinationSide_ = static_cast<core::ChannelCount>(wrap(destinationSide_, 2)); break;
        default: break;
    }
    dirty_.set();
}

NavigationRequest CopyChannelScreen::handleEvent(const events::Event& event) {
    if (const auto* encoder = std::get_if<events::EncoderEvent>(&event)) {
        change(encoder->delta);
        return NavigationRequest::none();
    }
    const auto* button = std::get_if<events::ButtonEvent>(&event);
    if (!button || button->action != core::ButtonAction::Pressed) return NavigationRequest::ignored();
    if (button->id == core::ButtonId::Back) {
        if (step_ == 0) return NavigationRequest::back();
        --step_; dirty_.set(); return NavigationRequest::none();
    }
    if (button->id != core::ButtonId::EncoderPush) return NavigationRequest::ignored();
    if (step_ + 1 < kStepCount) {
        ++step_; dirty_.set(); return NavigationRequest::none();
    }
    if (!projects_.hasProject()) return NavigationRequest::back();
    const auto& decks = projects_.project().decks();
    if (sourceDeck_ >= decks.size() || destinationDeck_ >= decks.size()) return NavigationRequest::back();
    auto status = projects_.execute(std::make_unique<project::CopyChannelCommand>(
        decks[sourceDeck_].id, sourceTrack_, sourceSide_, decks[destinationDeck_].id,
        destinationTrack_, destinationSide_));
    if (!status) {
        error_ = status.error().code(); dirty_.set(); return NavigationRequest::none();
    }
    return NavigationRequest::back();
}

void CopyChannelScreen::render(display::Painter& painter) {
    painter.clear(display::palette::kBackground);
    const auto& font = display::smallFont();
    painter.fillRect({0, 0, 128, 12}, display::palette::kSurface);
    painter.drawText(2, 0, "COPY CHANNEL", font, display::palette::kText);
    if (error_ != core::ErrorCode::Unknown) {
        painter.drawTextIn({0, 26, 128, 12}, core::toString(error_), font,
                           display::palette::kText, display::TextAlign::Center);
        dirty_.clear(); return;
    }
    if (!projects_.hasProject()) {
        painter.drawTextIn({0, 26, 128, 12}, "NO PROJECT", font,
                           display::palette::kText, display::TextAlign::Center);
        dirty_.clear(); return;
    }
    const auto& decks = projects_.project().decks();
    static constexpr const char* labels[kStepCount] = {
        "SOURCE DECK", "SOURCE TRACK", "SOURCE SIDE", "DEST DECK",
        "DEST TRACK", "DEST SIDE", "APPLY COPY"
    };
    painter.drawTextIn({0, 16, 128, 12}, labels[step_], font,
                       display::palette::kTextDim, display::TextAlign::Center);
    char value[20]{};
    switch (step_) {
        case 0: std::snprintf(value, sizeof(value), "%s", decks[sourceDeck_].name.c_str()); break;
        case 1: std::snprintf(value, sizeof(value), "TRACK %zu", sourceTrack_ + 1); break;
        case 2: std::snprintf(value, sizeof(value), "%s", sourceSide_ == 0 ? "LEFT" : "RIGHT"); break;
        case 3: std::snprintf(value, sizeof(value), "%s", decks[destinationDeck_].name.c_str()); break;
        case 4: std::snprintf(value, sizeof(value), "TRACK %zu", destinationTrack_ + 1); break;
        case 5: std::snprintf(value, sizeof(value), "%s", destinationSide_ == 0 ? "LEFT" : "RIGHT"); break;
        default: std::snprintf(value, sizeof(value), "PUSH TO COPY"); break;
    }
    painter.drawTextIn({0, 32, 128, 12}, value, font, display::palette::kText,
                       display::TextAlign::Center);
    painter.drawTextIn({0, 49, 128, 12}, "TURN / PUSH NEXT", font,
                       display::palette::kTextDim, display::TextAlign::Center);
    dirty_.clear();
}

}  // namespace aw::ui
