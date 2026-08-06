#include "aw/platform/rpi/GpioInput.hpp"

#include <atomic>

// Phase 6 replaces these stubs with libgpiod edge handling.

namespace aw::platform::rpi {

GpioPinMap GpioPinMap::prototype() noexcept {
    GpioPinMap map;
    // Prototype wiring on the Pi 5's 40-pin header (BCM numbering).  Kept
    // together so a rewiring is one diff; see docs/hardware.md for the physical
    // layout and the KY-040 notes.
    //
    // Only the encoder is wired on the current prototype: CLK -> 17, DT -> 27,
    // SW -> 22.  The remaining button lines below are the intended assignment
    // for when physical switches are added; until then those controls come from
    // the touch strip or the keyboard, merged in by `CompositeButtons`.
    map.buttonLines[static_cast<std::size_t>(core::ButtonId::Play)] = 5;
    map.buttonLines[static_cast<std::size_t>(core::ButtonId::Stop)] = 6;
    map.buttonLines[static_cast<std::size_t>(core::ButtonId::Record)] = 13;
    map.buttonLines[static_cast<std::size_t>(core::ButtonId::Import)] = 19;
    map.buttonLines[static_cast<std::size_t>(core::ButtonId::Export)] = 26;
    map.buttonLines[static_cast<std::size_t>(core::ButtonId::Undo)] = 12;
    map.buttonLines[static_cast<std::size_t>(core::ButtonId::Save)] = 16;
    map.buttonLines[static_cast<std::size_t>(core::ButtonId::Back)] = 20;
    map.buttonLines[static_cast<std::size_t>(core::ButtonId::Track1)] = 21;
    map.buttonLines[static_cast<std::size_t>(core::ButtonId::Track2)] = 22;
    map.buttonLines[static_cast<std::size_t>(core::ButtonId::Track3)] = 23;
    map.buttonLines[static_cast<std::size_t>(core::ButtonId::Track4)] = 24;
    // KY-040: CLK -> encoder A, DT -> encoder B, SW -> the push switch.
    // All three need pull-ups; the module's own resistors are unreliable, so
    // Phase 6 requests GPIO_V2_LINE_FLAG_BIAS_PULL_UP from libgpiod rather than
    // depending on the board.
    map.buttonLines[static_cast<std::size_t>(core::ButtonId::EncoderPush)] = 22;  // SW
    map.encoderLineA = 17;                                                        // CLK
    map.encoderLineB = 27;                                                        // DT
    return map;
}

// --- Buttons ----------------------------------------------------------------

struct GpioButtons::Impl {
    GpioPinMap pins;
    hal::ButtonListener* listener{nullptr};
    std::array<bool, core::kButtonCount> pressed{};
};

GpioButtons::GpioButtons(GpioPinMap pins) : impl_{std::make_unique<Impl>()} {
    impl_->pins = std::move(pins);
}

GpioButtons::~GpioButtons() = default;

void GpioButtons::setListener(hal::ButtonListener* listener) noexcept {
    impl_->listener = listener;
}

core::Status GpioButtons::start() {
    return core::notImplemented("GpioButtons::start (Phase 6)");
}

void GpioButtons::stop() noexcept { impl_->listener = nullptr; }

bool GpioButtons::isPressed(core::ButtonId id) const noexcept {
    const auto index = static_cast<std::size_t>(id);
    return index < impl_->pressed.size() && impl_->pressed[index];
}

// --- Encoder ----------------------------------------------------------------

struct GpioEncoder::Impl {
    GpioPinMap pins;
    hal::EncoderListener* listener{nullptr};
    std::atomic<std::int32_t> accumulatedDelta{0};
};

GpioEncoder::GpioEncoder(GpioPinMap pins) : impl_{std::make_unique<Impl>()} {
    impl_->pins = std::move(pins);
}

GpioEncoder::~GpioEncoder() = default;

void GpioEncoder::setListener(hal::EncoderListener* listener) noexcept {
    impl_->listener = listener;
}

core::Status GpioEncoder::start() {
    return core::notImplemented("GpioEncoder::start (Phase 6)");
}

void GpioEncoder::stop() noexcept { impl_->listener = nullptr; }

std::int32_t GpioEncoder::consumeDelta() noexcept {
    return impl_->accumulatedDelta.exchange(0, std::memory_order_relaxed);
}

}  // namespace aw::platform::rpi
