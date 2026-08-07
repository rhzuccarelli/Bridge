#include "aw/platform/rpi/GpioInput.hpp"

#include <gpiod.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

// Phase 6 replaces these stubs with libgpiod edge handling.

namespace aw::platform::rpi {

namespace {
std::uint64_t nowMicros() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

gpiod_line_request* requestLines(gpiod_chip* chip,
                                 const std::vector<unsigned int>& offsets,
                                 const char* consumer,
                                 gpiod_line_direction direction,
                                 bool edges) {
    gpiod_line_settings* settings = gpiod_line_settings_new();
    gpiod_line_config* lineConfig = gpiod_line_config_new();
    gpiod_request_config* requestConfig = gpiod_request_config_new();
    if (settings == nullptr || lineConfig == nullptr || requestConfig == nullptr) {
        gpiod_line_settings_free(settings);
        gpiod_line_config_free(lineConfig);
        gpiod_request_config_free(requestConfig);
        return nullptr;
    }
    gpiod_line_settings_set_direction(settings, direction);
    if (direction == GPIOD_LINE_DIRECTION_INPUT) {
        gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);
        if (edges) gpiod_line_settings_set_edge_detection(settings, GPIOD_LINE_EDGE_BOTH);
    } else {
        gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_ACTIVE);
    }
    gpiod_request_config_set_consumer(requestConfig, consumer);
    const int configured = gpiod_line_config_add_line_settings(
        lineConfig, offsets.data(), offsets.size(), settings);
    gpiod_line_request* request = configured < 0
                                      ? nullptr
                                      : gpiod_chip_request_lines(chip, requestConfig, lineConfig);
    gpiod_request_config_free(requestConfig);
    gpiod_line_config_free(lineConfig);
    gpiod_line_settings_free(settings);
    return request;
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
    gpiod_chip* chip{nullptr};
    gpiod_line_request* request{nullptr};
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
    const std::string chipPath = "/dev/" + impl_->pins.chipName;
    impl_->chip = gpiod_chip_open(chipPath.c_str());
    if (impl_->chip == nullptr) {
        return core::makeError(core::ErrorCode::DeviceNotConnected, "open GPIO chip");
    }
    std::vector<unsigned int> offsets;
    std::vector<std::size_t> indices;
    for (std::size_t index = 0; index < impl_->pins.buttonLines.size(); ++index) {
        const int offset = impl_->pins.buttonLines[index];
        if (offset < 0) continue;
        offsets.push_back(static_cast<unsigned int>(offset));
        indices.push_back(index);
    }
    if (offsets.empty()) {
        stop();
        return core::makeError(core::ErrorCode::DeviceNotConnected, "no GPIO buttons configured");
    }
    impl_->request = requestLines(impl_->chip, offsets, "audio-workstation-buttons",
                                  GPIOD_LINE_DIRECTION_INPUT, true);
    if (impl_->request == nullptr) {
        stop();
        return core::makeError(core::ErrorCode::DeviceBusy, "request GPIO button lines");
    }
    for (std::size_t item = 0; item < offsets.size(); ++item) {
        const bool raw = gpiod_line_request_get_value(impl_->request, offsets[item]) ==
                         GPIOD_LINE_VALUE_INACTIVE;
        (void)impl_->states[indices[item]].update(raw, nowMicros());
    }
    impl_->thread = std::jthread{[this, offsets = std::move(offsets),
                                  indices = std::move(indices)](std::stop_token stopToken) {
        gpiod_edge_event_buffer* events = gpiod_edge_event_buffer_new(16);
        if (events == nullptr) return;
        while (!stopToken.stop_requested()) {
            if (gpiod_line_request_wait_edge_events(impl_->request, 10'000'000) > 0) {
                (void)gpiod_line_request_read_edge_events(impl_->request, events, 16);
            }
            const std::uint64_t timestamp = nowMicros();
            for (std::size_t item = 0; item < offsets.size(); ++item) {
                const std::size_t index = indices[item];
                const bool raw = gpiod_line_request_get_value(impl_->request, offsets[item]) ==
                                 GPIOD_LINE_VALUE_INACTIVE;
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
        gpiod_edge_event_buffer_free(events);
    }};
    return core::success();
}

void GpioButtons::stop() noexcept {
    if (impl_->thread.joinable()) {
        impl_->thread.request_stop();
        impl_->thread.join();
    }
    if (impl_->request != nullptr) gpiod_line_request_release(impl_->request);
    impl_->request = nullptr;
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
    gpiod_line_request* request{nullptr};
    gpiod_line_request* powerRequest{nullptr};
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
    const std::string chipPath = "/dev/" + impl_->pins.chipName;
    impl_->chip = gpiod_chip_open(chipPath.c_str());
    if (impl_->chip == nullptr) {
        return core::makeError(core::ErrorCode::DeviceNotConnected, "open GPIO chip");
    }
    if (impl_->pins.encoderPowerLine >= 0) {
        impl_->powerRequest = requestLines(
            impl_->chip, {static_cast<unsigned int>(impl_->pins.encoderPowerLine)},
            "audio-workstation-encoder-power", GPIOD_LINE_DIRECTION_OUTPUT, false);
        if (impl_->powerRequest == nullptr) {
            stop();
            return core::makeError(core::ErrorCode::DeviceBusy, "request encoder power GPIO");
        }
    }
    const std::vector<unsigned int> offsets{
        static_cast<unsigned int>(impl_->pins.encoderLineA),
        static_cast<unsigned int>(impl_->pins.encoderLineB)};
    impl_->request = requestLines(impl_->chip, offsets, "audio-workstation-encoder",
                                  GPIOD_LINE_DIRECTION_INPUT, true);
    if (impl_->request == nullptr) {
        stop();
        return core::makeError(core::ErrorCode::DeviceBusy, "request encoder GPIO lines");
    }
    impl_->decoder.reset(
        gpiod_line_request_get_value(impl_->request, offsets[0]) == GPIOD_LINE_VALUE_ACTIVE,
        gpiod_line_request_get_value(impl_->request, offsets[1]) == GPIOD_LINE_VALUE_ACTIVE);
    impl_->thread = std::jthread{[this](std::stop_token stopToken) {
        gpiod_edge_event_buffer* events = gpiod_edge_event_buffer_new(16);
        if (events == nullptr) return;
        while (!stopToken.stop_requested()) {
            if (gpiod_line_request_wait_edge_events(impl_->request, 10'000'000) <= 0) continue;
            (void)gpiod_line_request_read_edge_events(impl_->request, events, 16);
            const std::int32_t delta = impl_->decoder.update(
                gpiod_line_request_get_value(impl_->request,
                                             static_cast<unsigned int>(impl_->pins.encoderLineA)) ==
                    GPIOD_LINE_VALUE_ACTIVE,
                gpiod_line_request_get_value(impl_->request,
                                             static_cast<unsigned int>(impl_->pins.encoderLineB)) ==
                    GPIOD_LINE_VALUE_ACTIVE);
            if (delta != 0) {
                impl_->accumulatedDelta.fetch_add(delta, std::memory_order_relaxed);
                if (impl_->listener != nullptr) {
                    impl_->listener->onEncoderTurned(delta, nowMicros());
                }
            }
        }
        gpiod_edge_event_buffer_free(events);
    }};
    return core::success();
}

void GpioEncoder::stop() noexcept {
    if (impl_->thread.joinable()) {
        impl_->thread.request_stop();
        impl_->thread.join();
    }
    if (impl_->request != nullptr) gpiod_line_request_release(impl_->request);
    if (impl_->powerRequest != nullptr) gpiod_line_request_release(impl_->powerRequest);
    impl_->request = nullptr;
    impl_->powerRequest = nullptr;
    if (impl_->chip != nullptr) gpiod_chip_close(impl_->chip);
    impl_->chip = nullptr;
}

std::int32_t GpioEncoder::consumeDelta() noexcept {
    return impl_->accumulatedDelta.exchange(0, std::memory_order_relaxed);
}

}  // namespace aw::platform::rpi
