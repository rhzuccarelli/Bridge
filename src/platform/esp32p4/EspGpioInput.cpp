#include "aw/platform/esp32p4/EspGpioInput.hpp"

#include <atomic>
#include <functional>
#include <utility>

#include "aw/platform/InputDecoding.hpp"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace aw::platform::esp32p4 {
namespace {

// Above the UI loop (app_main, priority 1) so a long redraw cannot stretch the
// sampling interval; the work per tick is a few GPIO reads.
constexpr UBaseType_t kPollPriority = 10;
constexpr std::uint32_t kPollStackBytes = 4096;

std::uint64_t nowMicros() noexcept { return static_cast<std::uint64_t>(esp_timer_get_time()); }

core::Status configureInput(int pin) {
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << static_cast<unsigned>(pin);
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&config) != ESP_OK) {
        return core::makeError(core::ErrorCode::InvalidArgument,
                               "cannot configure GPIO" + std::to_string(pin));
    }
    return core::success();
}

bool high(int pin) noexcept { return gpio_get_level(static_cast<gpio_num_t>(pin)) != 0; }

/// Calls `tick` once per millisecond on its own FreeRTOS task until stopped.
class PollTask {
public:
    ~PollTask() { stop(); }

    core::Status start(const char* name, std::function<void()> tick) {
        stop();
        tick_ = std::move(tick);
        exited_ = xSemaphoreCreateBinary();
        if (exited_ == nullptr) return core::makeError(core::ErrorCode::OutOfMemory, name);
        running_.store(true);
        if (xTaskCreate(&PollTask::run, name, kPollStackBytes, this, kPollPriority, nullptr) !=
            pdPASS) {
            running_.store(false);
            vSemaphoreDelete(exited_);
            exited_ = nullptr;
            return core::makeError(core::ErrorCode::OutOfMemory, name);
        }
        return core::success();
    }

    void stop() noexcept {
        if (exited_ == nullptr) return;
        running_.store(false);
        xSemaphoreTake(exited_, portMAX_DELAY);
        vSemaphoreDelete(exited_);
        exited_ = nullptr;
    }

private:
    static void run(void* context) {
        auto& self = *static_cast<PollTask*>(context);
        const TickType_t period = pdMS_TO_TICKS(1) > 0 ? pdMS_TO_TICKS(1) : 1;
        TickType_t wake = xTaskGetTickCount();
        while (self.running_.load()) {
            self.tick_();
            xTaskDelayUntil(&wake, period);
        }
        xSemaphoreGive(self.exited_);
        vTaskDelete(nullptr);
    }

    std::function<void()> tick_;
    std::atomic<bool> running_{false};
    SemaphoreHandle_t exited_{nullptr};
};

}  // namespace

EspPinMap EspPinMap::picoPrototype() noexcept {
    EspPinMap map;
    map.buttonLines.fill(-1);
    map.buttonLines[static_cast<std::size_t>(core::ButtonId::EncoderPush)] = 17;  // SW
    map.encoderLineA = 16;                                                        // CLK
    map.encoderLineB = 15;                                                        // DT
    return map;
}

// --- Buttons ----------------------------------------------------------------

struct EspGpioButtons::Impl {
    EspPinMap pins;
    std::atomic<hal::ButtonListener*> listener{nullptr};
    std::array<ButtonStateMachine, core::kButtonCount> states{};
    std::array<std::atomic<bool>, core::kButtonCount> pressed{};
    PollTask task;

    void tick() {
        const std::uint64_t now = nowMicros();
        for (std::size_t i = 0; i < core::kButtonCount; ++i) {
            const int pin = pins.buttonLines[i];
            if (pin < 0) continue;
            const auto action = states[i].update(!high(pin), now);  // Active low.
            if (!action) continue;
            pressed[i].store(states[i].isPressed(), std::memory_order_relaxed);
            if (hal::ButtonListener* target = listener.load()) {
                target->onButtonEvent(static_cast<core::ButtonId>(i), *action, now);
            }
        }
    }
};

EspGpioButtons::EspGpioButtons(EspPinMap pins) : impl_{std::make_unique<Impl>()} {
    impl_->pins = pins;
}

EspGpioButtons::~EspGpioButtons() { stop(); }

void EspGpioButtons::setListener(hal::ButtonListener* listener) noexcept {
    impl_->listener.store(listener);
}

core::Status EspGpioButtons::start() {
    for (const int pin : impl_->pins.buttonLines) {
        if (pin < 0) continue;
        if (auto status = configureInput(pin); !status) return status;
    }
    return impl_->task.start("aw_buttons", [this] { impl_->tick(); });
}

void EspGpioButtons::stop() noexcept { impl_->task.stop(); }

bool EspGpioButtons::isPressed(core::ButtonId id) const noexcept {
    const auto index = static_cast<std::size_t>(id);
    return index < core::kButtonCount && impl_->pressed[index].load(std::memory_order_relaxed);
}

// --- Encoder ----------------------------------------------------------------

struct EspGpioEncoder::Impl {
    EspPinMap pins;
    std::atomic<hal::EncoderListener*> listener{nullptr};
    std::atomic<std::int32_t> accumulatedDelta{0};
    QuadratureDecoder decoder;
    PollTask task;

    void tick() {
        const std::int32_t delta = decoder.update(high(pins.encoderLineA), high(pins.encoderLineB));
        if (delta == 0) return;
        accumulatedDelta.fetch_add(delta, std::memory_order_relaxed);
        if (hal::EncoderListener* target = listener.load()) {
            target->onEncoderTurned(delta, nowMicros());
        }
    }
};

EspGpioEncoder::EspGpioEncoder(EspPinMap pins) : impl_{std::make_unique<Impl>()} {
    impl_->pins = pins;
}

EspGpioEncoder::~EspGpioEncoder() { stop(); }

void EspGpioEncoder::setListener(hal::EncoderListener* listener) noexcept {
    impl_->listener.store(listener);
}

core::Status EspGpioEncoder::start() {
    if (impl_->pins.encoderLineA < 0 || impl_->pins.encoderLineB < 0) {
        return core::makeError(core::ErrorCode::DeviceNotConnected, "encoder not wired");
    }
    if (auto status = configureInput(impl_->pins.encoderLineA); !status) return status;
    if (auto status = configureInput(impl_->pins.encoderLineB); !status) return status;
    impl_->decoder.reset(high(impl_->pins.encoderLineA), high(impl_->pins.encoderLineB));
    return impl_->task.start("aw_encoder", [this] { impl_->tick(); });
}

void EspGpioEncoder::stop() noexcept { impl_->task.stop(); }

std::int32_t EspGpioEncoder::consumeDelta() noexcept {
    return impl_->accumulatedDelta.exchange(0, std::memory_order_relaxed);
}

}  // namespace aw::platform::esp32p4
