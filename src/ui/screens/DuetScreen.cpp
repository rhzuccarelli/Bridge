#include "aw/ui/screens/DuetScreen.hpp"

#include "aw/core/Logger.hpp"
#include "aw/display/Color.hpp"
#include "aw/display/Font.hpp"

#include <algorithm>
#include <string>

namespace aw::ui {

DuetScreen::DuetScreen(devices::ApogeeDuet& duet, TaskRunner& tasks,
                       events::EventQueue& workerQueue) noexcept
    : duet_{duet}, tasks_{tasks}, workerQueue_{workerQueue} {
    for (auto& type : types_) type.store(-1, std::memory_order_relaxed);
}

void DuetScreen::onEnter() {
    selection_ = 0;
    submit();
    dirty_.set();
}

void DuetScreen::submit(int input, devices::DuetInputType type) {
    stage_ = Stage::Busy;
    const std::uint32_t jobId = ++jobId_;
    const core::Status accepted = tasks_.submit([this, jobId, input, type] {
        core::Status result = core::success();
        if (input >= 0) {
            result = duet_.setInputType(input, type);
            if (result) {
                core::log::info("duet", "input " + std::to_string(input + 1) + " set to " +
                                            std::string{devices::toString(type)});
            }
        }
        for (int each = 0; result && each < devices::ApogeeDuet::kInputCount; ++each) {
            auto read = duet_.inputType(each);
            if (!read) result = read.error();
            else types_[static_cast<std::size_t>(each)].store(static_cast<int>(read.value()),
                                                              std::memory_order_release);
        }
        if (result) {
            std::string inputs;
            for (const auto& each : types_)
                inputs += (inputs.empty() ? "" : ", ") +
                          std::string{devices::toString(static_cast<devices::DuetInputType>(
                              each.load(std::memory_order_acquire)))};
            core::log::info("duet", "inputs: " + inputs);
        } else {
            core::log::warning("duet", result.error().describe());
            for (auto& each : types_) each.store(-1, std::memory_order_release);  // Unknown now.
        }
        workerQueue_.post(events::JobFinishedEvent{
            events::JobKind::DeviceControl, jobId, static_cast<bool>(result),
            result ? core::ErrorCode::Unknown : result.error().code()});
    });
    if (!accepted) {
        stage_ = Stage::Ready;
        lastError_ = accepted.error().code();
    }
}

NavigationRequest DuetScreen::handleEvent(const events::Event& event) {
    if (const auto* finished = std::get_if<events::JobFinishedEvent>(&event)) {
        if (finished->kind != events::JobKind::DeviceControl || finished->jobId != jobId_)
            return NavigationRequest::ignored();
        stage_ = Stage::Ready;
        lastError_ = finished->succeeded ? core::ErrorCode::Unknown : finished->error;
        dirty_.set();
        return NavigationRequest::none();
    }
    if (const auto* encoder = std::get_if<events::EncoderEvent>(&event)) {
        const int delta = static_cast<int>(encoder->delta);
        if (stage_ == Stage::Editing) {
            pending_ = std::clamp(pending_ + delta, 0, devices::kDuetInputTypeCount - 1);
        } else {
            selection_ = std::clamp(selection_ + delta, 0, devices::ApogeeDuet::kInputCount - 1);
        }
        dirty_.set();
        return NavigationRequest::none();
    }
    const auto* button = std::get_if<events::ButtonEvent>(&event);
    if (button == nullptr || button->action != core::ButtonAction::Pressed)
        return NavigationRequest::ignored();
    if (button->id == core::ButtonId::Back && stage_ == Stage::Editing) {
        stage_ = Stage::Ready;
        dirty_.set();
        return NavigationRequest::none();
    }
    if (button->id != core::ButtonId::EncoderPush) return NavigationRequest::ignored();
    const int current = types_[static_cast<std::size_t>(selection_)].load(std::memory_order_acquire);
    if (stage_ == Stage::Ready) {
        if (current < 0) {
            submit();  // Nothing read yet (no Duet when the screen opened): try again.
        } else {
            pending_ = current;
            stage_ = Stage::Editing;
        }
    } else if (stage_ == Stage::Editing) {
        if (pending_ == current) stage_ = Stage::Ready;
        else submit(selection_, static_cast<devices::DuetInputType>(pending_));
    }
    dirty_.set();
    return NavigationRequest::none();
}

void DuetScreen::render(display::Painter& painter) {
    painter.clear(display::palette::kBackground);
    const auto& font = display::smallFont();
    painter.fillRect({0, 0, 128, 12}, display::palette::kSurface);
    painter.drawText(2, 0, "DUET INPUTS", font, display::palette::kText);
    for (int input = 0; input < devices::ApogeeDuet::kInputCount; ++input) {
        const auto y = static_cast<std::int16_t>(16 + input * 13);
        const bool selected = input == selection_;
        if (selected)
            painter.drawRect({0, static_cast<std::int16_t>(y - 1), 128, 12}, display::palette::kAccent);
        painter.drawText(4, y, input == 0 ? "IN 1" : "IN 2", font, display::palette::kText);
        const int type = types_[static_cast<std::size_t>(input)].load(std::memory_order_acquire);
        std::string value = type < 0 ? "--" : std::string{devices::toString(
                                                   static_cast<devices::DuetInputType>(type))};
        if (selected && stage_ == Stage::Editing)
            value = "< " + std::string{devices::toString(
                               static_cast<devices::DuetInputType>(pending_))} + " >";
        painter.drawTextIn({40, y, 86, 12}, value, font, display::palette::kText,
                           display::TextAlign::Right);
    }
    std::string_view status = "PUSH TO CHANGE";
    if (stage_ == Stage::Busy) status = "TALKING TO DUET";
    else if (stage_ == Stage::Editing) status = "PUSH TO SET";
    else if (lastError_ == core::ErrorCode::DeviceNotConnected) status = "NO DUET - PUSH";
    else if (lastError_ != core::ErrorCode::Unknown) status = core::toString(lastError_);
    painter.drawTextIn({0, 50, 128, 12}, status, font, display::palette::kTextDim,
                       display::TextAlign::Center);
    dirty_.clear();
}

}  // namespace aw::ui
