#include "aw/platform/rpi/GpioInput.hpp"

#include <gpiod.h>

#include <atomic>
#include <chrono>
#include <poll.h>
#include <thread>
#include <vector>

// Phase 6 replaces these stubs with libgpiod edge handling.

namespace aw::platform::rpi {

namespace {
std::uint64_t nowMicros() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
}  // namespace

GpioPinMap GpioPinMap::prototype() noexcept {
    GpioPinMap map;
    map.buttonLines.fill(-1);
    // Prototype wiring on the Pi 5's 40-pin header (BCM numbering).  Kept
    // together so a rewiring is one diff; see docs/hardware.md for the physical
    // layout and the KY-040 notes.
    //
    // Only the encoder is wired on the current prototype: CLK -> 17, DT -> 27,
    // SW -> 22.  The remaining button lines below are the intended assignment
    // for when physical switches are added; until then those controls come from
    // the touch strip or the keyboard, merged in by `CompositeButtons`.
    // KY-040, on the original clean assignment near the start of the header:
    //
    //   pin 11 / GPIO 17  CLK
    //   pin 13 / GPIO 27  DT
    //   pin 15 / GPIO 22  SW
    //   pin 1             +    (real 3.3 V rail)
    //   pin 6 or 9        GND
    //
    // This layout is only reachable when the 40-pin header is *not* covered by a
    // pin-hungry HAT - i.e. when the display is a jumper-wired module (the SPI
    // OLED) or the HAT is lifted on a stacking header.  With the original 3.5"
    // LCD HAT sitting directly on pins 1-26 these lines are buried, which is why
    // an earlier revision moved the encoder to the 29-39 tail; see the project
    // history if that arrangement is ever needed again.
    //
    // All three signal lines request BIAS_PULL_UP from libgpiod regardless of
    // the module's own resistors - it costs nothing and tolerates a module whose
    // pull-ups are absent.
    map.buttonLines[static_cast<std::size_t>(core::ButtonId::EncoderPush)] = 22;  // SW
    map.encoderLineA = 17;                                                        // CLK
    map.encoderLineB = 27;                                                        // DT
    map.encoderPowerLine = -1;  // + wired to the 3.3 V pin (pin 1), not a GPIO.
    return map;
}

// --- Buttons ----------------------------------------------------------------

struct GpioButtons::Impl {
    GpioPinMap pins;
    hal::ButtonListener* listener{nullptr};
    std::array<std::atomic<bool>, core::kButtonCount> pressed{};
    std::array<ButtonStateMachine, core::kButtonCount> states{};
    std::array<gpiod_line*, core::kButtonCount> lines{};
    gpiod_chip* chip{nullptr};
    std::jthread thread;
};

GpioButtons::GpioButtons(GpioPinMap pins) : impl_{std::make_unique<Impl>()} {
    impl_->pins = std::move(pins);
}

GpioButtons::~GpioButtons() { stop(); }

void GpioButtons::setListener(hal::ButtonListener* listener) noexcept {
    impl_->listener = listener;
}

core::Status GpioButtons::start() {
    stop();
    impl_->chip = gpiod_chip_open_by_name(impl_->pins.chipName.c_str());
    if (impl_->chip == nullptr) {
        return core::makeError(core::ErrorCode::DeviceNotConnected, "open GPIO chip");
    }
    std::vector<pollfd> descriptors;
    std::vector<std::size_t> indices;
    for (std::size_t index = 0; index < impl_->pins.buttonLines.size(); ++index) {
        const int offset = impl_->pins.buttonLines[index];
        if (offset < 0) continue;
        gpiod_line* line = gpiod_chip_get_line(impl_->chip, static_cast<unsigned int>(offset));
        if (line == nullptr ||
            gpiod_line_request_both_edges_events_flags(
                line, "audio-workstation-buttons", GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP) < 0) {
            stop();
            return core::makeError(core::ErrorCode::DeviceBusy, "request GPIO button line");
        }
        impl_->lines[index] = line;
        descriptors.push_back(pollfd{gpiod_line_event_get_fd(line), POLLIN, 0});
        indices.push_back(index);
        const bool raw = gpiod_line_get_value(line) == 0;
        (void)impl_->states[index].update(raw, nowMicros());
    }
    if (descriptors.empty()) {
        stop();
        return core::makeError(core::ErrorCode::DeviceNotConnected, "no GPIO buttons configured");
    }
    impl_->thread = std::jthread{[this, descriptors = std::move(descriptors),
                                  indices = std::move(indices)](std::stop_token stopToken) mutable {
        while (!stopToken.stop_requested()) {
            (void)::poll(descriptors.data(), descriptors.size(), 10);
            const std::uint64_t timestamp = nowMicros();
            for (std::size_t item = 0; item < descriptors.size(); ++item) {
                if ((descriptors[item].revents & POLLIN) != 0) {
                    gpiod_line_event event{};
                    (void)gpiod_line_event_read(impl_->lines[indices[item]], &event);
                    descriptors[item].revents = 0;
                }
                const std::size_t index = indices[item];
                const bool raw = gpiod_line_get_value(impl_->lines[index]) == 0;
                if (const auto action = impl_->states[index].update(raw, timestamp)) {
                    impl_->pressed[index].store(impl_->states[index].isPressed(),
                                                std::memory_order_release);
                    if (impl_->listener != nullptr) {
                        impl_->listener->onButtonEvent(static_cast<core::ButtonId>(index),
                                                       *action, timestamp);
                    }
                }
            }
        }
    }};
    return core::success();
}

void GpioButtons::stop() noexcept {
    if (impl_->thread.joinable()) {
        impl_->thread.request_stop();
        impl_->thread.join();
    }
    for (gpiod_line*& line : impl_->lines) {
        if (line != nullptr) gpiod_line_release(line);
        line = nullptr;
    }
    if (impl_->chip != nullptr) gpiod_chip_close(impl_->chip);
    impl_->chip = nullptr;
}

bool GpioButtons::isPressed(core::ButtonId id) const noexcept {
    const auto index = static_cast<std::size_t>(id);
    return index < impl_->pressed.size() &&
           impl_->pressed[index].load(std::memory_order_acquire);
}

// --- Encoder ----------------------------------------------------------------

struct GpioEncoder::Impl {
    GpioPinMap pins;
    hal::EncoderListener* listener{nullptr};
    std::atomic<std::int32_t> accumulatedDelta{0};
    gpiod_chip* chip{nullptr};
    gpiod_line* lineA{nullptr};
    gpiod_line* lineB{nullptr};
    gpiod_line* powerLine{nullptr};
    QuadratureDecoder decoder;
    std::jthread thread;
};

GpioEncoder::GpioEncoder(GpioPinMap pins) : impl_{std::make_unique<Impl>()} {
    impl_->pins = std::move(pins);
}

GpioEncoder::~GpioEncoder() { stop(); }

void GpioEncoder::setListener(hal::EncoderListener* listener) noexcept {
    impl_->listener = listener;
}

core::Status GpioEncoder::start() {
    stop();
    impl_->chip = gpiod_chip_open_by_name(impl_->pins.chipName.c_str());
    if (impl_->chip == nullptr) {
        return core::makeError(core::ErrorCode::DeviceNotConnected, "open GPIO chip");
    }
    if (impl_->pins.encoderPowerLine >= 0) {
        impl_->powerLine = gpiod_chip_get_line(
            impl_->chip, static_cast<unsigned int>(impl_->pins.encoderPowerLine));
        if (impl_->powerLine == nullptr ||
            gpiod_line_request_output(impl_->powerLine, "audio-workstation-encoder-power", 1) < 0) {
            stop();
            return core::makeError(core::ErrorCode::DeviceBusy, "request encoder power GPIO");
        }
    }
    impl_->lineA = gpiod_chip_get_line(impl_->chip,
                                       static_cast<unsigned int>(impl_->pins.encoderLineA));
    impl_->lineB = gpiod_chip_get_line(impl_->chip,
                                       static_cast<unsigned int>(impl_->pins.encoderLineB));
    if (impl_->lineA == nullptr || impl_->lineB == nullptr ||
        gpiod_line_request_both_edges_events_flags(
            impl_->lineA, "audio-workstation-encoder", GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP) < 0 ||
        gpiod_line_request_both_edges_events_flags(
            impl_->lineB, "audio-workstation-encoder", GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP) < 0) {
        stop();
        return core::makeError(core::ErrorCode::DeviceBusy, "request encoder GPIO lines");
    }
    impl_->decoder.reset(gpiod_line_get_value(impl_->lineA) != 0,
                         gpiod_line_get_value(impl_->lineB) != 0);
    impl_->thread = std::jthread{[this](std::stop_token stopToken) {
        std::array<pollfd, 2> descriptors{{
            {gpiod_line_event_get_fd(impl_->lineA), POLLIN, 0},
            {gpiod_line_event_get_fd(impl_->lineB), POLLIN, 0}}};
        while (!stopToken.stop_requested()) {
            if (::poll(descriptors.data(), descriptors.size(), 10) <= 0) continue;
            for (std::size_t index = 0; index < descriptors.size(); ++index) {
                if ((descriptors[index].revents & POLLIN) == 0) continue;
                gpiod_line_event event{};
                (void)gpiod_line_event_read(index == 0 ? impl_->lineA : impl_->lineB, &event);
                descriptors[index].revents = 0;
            }
            const std::int32_t delta = impl_->decoder.update(
                gpiod_line_get_value(impl_->lineA) != 0,
                gpiod_line_get_value(impl_->lineB) != 0);
            if (delta != 0) {
                impl_->accumulatedDelta.fetch_add(delta, std::memory_order_relaxed);
                if (impl_->listener != nullptr) {
                    impl_->listener->onEncoderTurned(delta, nowMicros());
                }
            }
        }
    }};
    return core::success();
}

void GpioEncoder::stop() noexcept {
    if (impl_->thread.joinable()) {
        impl_->thread.request_stop();
        impl_->thread.join();
    }
    if (impl_->lineA != nullptr) gpiod_line_release(impl_->lineA);
    if (impl_->lineB != nullptr) gpiod_line_release(impl_->lineB);
    if (impl_->powerLine != nullptr) gpiod_line_release(impl_->powerLine);
    impl_->lineA = nullptr;
    impl_->lineB = nullptr;
    impl_->powerLine = nullptr;
    if (impl_->chip != nullptr) gpiod_chip_close(impl_->chip);
    impl_->chip = nullptr;
}

std::int32_t GpioEncoder::consumeDelta() noexcept {
    return impl_->accumulatedDelta.exchange(0, std::memory_order_relaxed);
}

}  // namespace aw::platform::rpi
