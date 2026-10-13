#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "aw/core/Controls.hpp"

namespace aw::platform {

/// Debounce (20 ms), long press (500 ms) and auto-repeat (every 100 ms) for one
/// switch, from raw level samples. Pure logic: the Raspberry Pi feeds it from
/// libgpiod edge events, the ESP32-P4 from a 1 kHz poll.
class ButtonStateMachine {
public:
    [[nodiscard]] std::optional<core::ButtonAction> update(bool rawPressed,
                                                            std::uint64_t nowMicros) noexcept {
        if (rawPressed != rawPressed_) {
            rawPressed_ = rawPressed;
            rawChangedAt_ = nowMicros;
        }
        if (stablePressed_ != rawPressed_ && nowMicros - rawChangedAt_ >= 20'000) {
            stablePressed_ = rawPressed_;
            if (stablePressed_) {
                pressedAt_ = nowMicros;
                lastRepeatAt_ = nowMicros;
                longPressSent_ = false;
                return core::ButtonAction::Pressed;
            }
            longPressSent_ = false;
            return core::ButtonAction::Released;
        }
        if (stablePressed_ && !longPressSent_ && nowMicros - pressedAt_ >= 500'000) {
            longPressSent_ = true;
            lastRepeatAt_ = nowMicros;
            return core::ButtonAction::LongPressed;
        }
        if (stablePressed_ && longPressSent_ && nowMicros - lastRepeatAt_ >= 100'000) {
            lastRepeatAt_ = nowMicros;
            return core::ButtonAction::Repeat;
        }
        return std::nullopt;
    }
    [[nodiscard]] bool isPressed() const noexcept { return stablePressed_; }

private:
    bool rawPressed_{false};
    bool stablePressed_{false};
    bool longPressSent_{false};
    std::uint64_t rawChangedAt_{0};
    std::uint64_t pressedAt_{0};
    std::uint64_t lastRepeatAt_{0};
};

/// Quadrature state table for a rotary encoder. Coalesces the four transitions
/// of one detent into one step: +1 clockwise (A leads B), -1 counter-clockwise.
class QuadratureDecoder {
public:
    void reset(bool a, bool b) noexcept {
        previous_ = static_cast<std::uint8_t>((a ? 2U : 0U) | (b ? 1U : 0U));
        quarterSteps_ = 0;
    }
    [[nodiscard]] std::int32_t update(bool a, bool b) noexcept {
        static constexpr std::array<std::int8_t, 16> transitions{
            0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
        const auto current = static_cast<std::uint8_t>((a ? 2U : 0U) | (b ? 1U : 0U));
        quarterSteps_ += transitions[static_cast<std::size_t>((previous_ << 2U) | current)];
        previous_ = current;
        if (quarterSteps_ >= 4) { quarterSteps_ = 0; return 1; }
        if (quarterSteps_ <= -4) { quarterSteps_ = 0; return -1; }
        return 0;
    }

private:
    std::uint8_t previous_{0};
    std::int32_t quarterSteps_{0};
};

}  // namespace aw::platform
