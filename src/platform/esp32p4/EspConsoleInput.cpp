#include "aw/platform/esp32p4/EspConsoleInput.hpp"

#include <mutex>

#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace aw::platform::esp32p4 {
namespace {

constexpr auto kUart = static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM);

}  // namespace

struct EspConsoleInput::Impl {
    std::mutex mutex;
    hal::ButtonListener* buttons{nullptr};
    hal::EncoderListener* encoder{nullptr};
    std::atomic<std::int32_t> delta{0};
    bool started{false};
    int escape{0};  ///< Position in an ESC [ x arrow sequence.

    core::Status start() {
        const std::lock_guard lock{mutex};
        if (started) return core::success();
        if (!uart_is_driver_installed(kUart) &&
            uart_driver_install(kUart, 256, 0, 0, nullptr, 0) != ESP_OK) {
            return core::makeError(core::ErrorCode::DeviceNotConnected, "console UART");
        }
        if (xTaskCreate(&Impl::run, "console_keys", 3072, this, 2, nullptr) != pdPASS) {
            return core::makeError(core::ErrorCode::OutOfMemory, "console input task");
        }
        started = true;
        return core::success();
    }

    static void run(void* context) {
        auto& self = *static_cast<Impl*>(context);
        for (;;) {
            std::uint8_t byte = 0;
            if (uart_read_bytes(kUart, &byte, 1, portMAX_DELAY) == 1) self.key(byte);
        }
    }

    void press(core::ButtonId id) {
        const std::uint64_t now = static_cast<std::uint64_t>(esp_timer_get_time());
        hal::ButtonListener* listener = nullptr;
        {
            const std::lock_guard lock{mutex};
            listener = buttons;
        }
        if (listener == nullptr) return;
        listener->onButtonEvent(id, core::ButtonAction::Pressed, now);
        listener->onButtonEvent(id, core::ButtonAction::Released, now);
    }

    void turn(std::int32_t step) {
        delta.fetch_add(step, std::memory_order_relaxed);
        hal::EncoderListener* listener = nullptr;
        {
            const std::lock_guard lock{mutex};
            listener = encoder;
        }
        if (listener != nullptr)
            listener->onEncoderTurned(step, static_cast<std::uint64_t>(esp_timer_get_time()));
    }

    void key(std::uint8_t byte) {
        if (escape == 1) {
            escape = byte == '[' ? 2 : 0;
            if (escape == 0) press(core::ButtonId::Back);  // A lone Esc.
            return;
        }
        if (escape == 2) {
            escape = 0;
            if (byte == 'C' || byte == 'B') turn(1);
            if (byte == 'D' || byte == 'A') turn(-1);
            return;
        }
        switch (byte) {
            case 0x1B: escape = 1; return;
            case ' ': return press(core::ButtonId::Play);
            case 's': return press(core::ButtonId::Stop);
            case 'r': return press(core::ButtonId::Record);
            case 'i': return press(core::ButtonId::Import);
            case 'e': return press(core::ButtonId::Export);
            case 'u': return press(core::ButtonId::Undo);
            case 'w': return press(core::ButtonId::Save);
            case 'b': return press(core::ButtonId::Back);
            case '\r':
            case '\n': return press(core::ButtonId::EncoderPush);
            case 'j': return turn(1);
            case 'k': return turn(-1);
            default: break;
        }
        if (byte >= '1' && byte <= '6')
            press(static_cast<core::ButtonId>(static_cast<int>(core::ButtonId::Track1) + (byte - '1')));
    }
};

namespace {

class ConsoleButtons final : public hal::ButtonInterface {
public:
    explicit ConsoleButtons(std::shared_ptr<EspConsoleInput::Impl> impl) : impl_{std::move(impl)} {}
    void setListener(hal::ButtonListener* listener) noexcept override {
        const std::lock_guard lock{impl_->mutex};
        impl_->buttons = listener;
    }
    core::Status start() override { return impl_->start(); }
    void stop() noexcept override { setListener(nullptr); }
    bool isPressed(core::ButtonId) const noexcept override { return false; }

private:
    std::shared_ptr<EspConsoleInput::Impl> impl_;
};

class ConsoleEncoder final : public hal::EncoderInterface {
public:
    explicit ConsoleEncoder(std::shared_ptr<EspConsoleInput::Impl> impl) : impl_{std::move(impl)} {}
    void setListener(hal::EncoderListener* listener) noexcept override {
        const std::lock_guard lock{impl_->mutex};
        impl_->encoder = listener;
    }
    core::Status start() override { return impl_->start(); }
    void stop() noexcept override { setListener(nullptr); }
    std::int32_t consumeDelta() noexcept override { return impl_->delta.exchange(0); }

private:
    std::shared_ptr<EspConsoleInput::Impl> impl_;
};

}  // namespace

EspConsoleInput::EspConsoleInput() : impl_{std::make_shared<Impl>()} {}
EspConsoleInput::~EspConsoleInput() = default;

std::unique_ptr<hal::ButtonInterface> EspConsoleInput::buttons() {
    return std::make_unique<ConsoleButtons>(impl_);
}

std::unique_ptr<hal::EncoderInterface> EspConsoleInput::encoder() {
    return std::make_unique<ConsoleEncoder>(impl_);
}

}  // namespace aw::platform::esp32p4
