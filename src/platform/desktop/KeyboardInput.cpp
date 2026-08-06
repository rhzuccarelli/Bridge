#include "aw/platform/desktop/KeyboardInput.hpp"

#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

namespace aw::platform::desktop {
namespace {

constexpr int kStdin = STDIN_FILENO;

std::uint64_t nowMicros() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

}  // namespace

KeyMapping mapKey(int key) noexcept {
    switch (key) {
        case ' ': return KeyMapping{KeyAction::Button, core::ButtonId::Play};
        case 's': return KeyMapping{KeyAction::Button, core::ButtonId::Stop};
        case 'r': return KeyMapping{KeyAction::Button, core::ButtonId::Record};
        case 'i': return KeyMapping{KeyAction::Button, core::ButtonId::Import};
        case 'e': return KeyMapping{KeyAction::Button, core::ButtonId::Export};
        case 'u': return KeyMapping{KeyAction::Button, core::ButtonId::Undo};
        case 'w': return KeyMapping{KeyAction::Button, core::ButtonId::Save};

        case '1': return KeyMapping{KeyAction::Button, core::ButtonId::Track1};
        case '2': return KeyMapping{KeyAction::Button, core::ButtonId::Track2};
        case '3': return KeyMapping{KeyAction::Button, core::ButtonId::Track3};
        case '4': return KeyMapping{KeyAction::Button, core::ButtonId::Track4};

        case kKeyEnter: return KeyMapping{KeyAction::Button, core::ButtonId::EncoderPush};

        case 'b':
        case kKeyEscape:
        case kKeyBackspace:
            return KeyMapping{KeyAction::Button, core::ButtonId::Back};

        case kKeyRight:
        case kKeyDown:
            return KeyMapping{KeyAction::EncoderCw, core::ButtonId::Play};
        case kKeyLeft:
        case kKeyUp:
            return KeyMapping{KeyAction::EncoderCcw, core::ButtonId::Play};

        case 'q': return KeyMapping{KeyAction::Quit, core::ButtonId::Play};

        default: return KeyMapping{};
    }
}

struct KeyboardInputHub::Impl {
    hal::ButtonListener* buttonListener{nullptr};
    hal::EncoderListener* encoderListener{nullptr};
    std::atomic<std::int32_t> accumulatedDelta{0};
    std::atomic<bool> running{false};
    std::thread thread;
    termios originalTermios{};
    bool termiosSaved{false};

    void run();
    void dispatch(int key);
    [[nodiscard]] int readKey();
};

int KeyboardInputHub::Impl::readKey() {
    char byte = 0;
    if (::read(kStdin, &byte, 1) != 1) {
        return -1;
    }

    if (byte == '\r' || byte == '\n') {
        return kKeyEnter;
    }
    if (byte == 0x7F || byte == 0x08) {
        return kKeyBackspace;
    }
    if (byte != 0x1B) {
        return static_cast<unsigned char>(byte);
    }

    // ESC: could be a bare Escape or the start of a CSI sequence.  A short poll
    // distinguishes them; a bare Escape has nothing behind it.
    pollfd descriptor{kStdin, POLLIN, 0};
    if (::poll(&descriptor, 1, 40) <= 0) {
        return kKeyEscape;
    }

    char bracket = 0;
    if (::read(kStdin, &bracket, 1) != 1 || bracket != '[') {
        return kKeyEscape;
    }

    char code = 0;
    if (::read(kStdin, &code, 1) != 1) {
        return kKeyEscape;
    }
    switch (code) {
        case 'A': return kKeyUp;
        case 'B': return kKeyDown;
        case 'C': return kKeyRight;
        case 'D': return kKeyLeft;
        default: return kKeyEscape;
    }
}

void KeyboardInputHub::Impl::dispatch(int key) {
    const KeyMapping mapping = mapKey(key);
    const std::uint64_t timestamp = nowMicros();

    switch (mapping.action) {
        case KeyAction::None:
            return;

        case KeyAction::Button:
            if (buttonListener != nullptr) {
                // A terminal delivers keystrokes, not key-down/key-up, so a
                // press is synthesised as an immediate press+release pair.
                buttonListener->onButtonEvent(mapping.button, core::ButtonAction::Pressed,
                                              timestamp);
                buttonListener->onButtonEvent(mapping.button, core::ButtonAction::Released,
                                              timestamp);
            }
            return;

        case KeyAction::EncoderCw:
        case KeyAction::EncoderCcw: {
            const std::int32_t delta = mapping.action == KeyAction::EncoderCw ? 1 : -1;
            accumulatedDelta.fetch_add(delta, std::memory_order_relaxed);
            if (encoderListener != nullptr) {
                encoderListener->onEncoderTurned(delta, timestamp);
            }
            return;
        }

        case KeyAction::Quit:
            // Reuse the application's existing signal path rather than inventing
            // a second shutdown route.
            std::raise(SIGINT);
            return;
    }
}

void KeyboardInputHub::Impl::run() {
    while (running.load(std::memory_order_acquire)) {
        pollfd descriptor{kStdin, POLLIN, 0};
        if (::poll(&descriptor, 1, 100) <= 0) {
            continue;  // Timeout: re-check `running` so stop() is responsive.
        }
        const int key = readKey();
        if (key >= 0) {
            dispatch(key);
        }
    }
}

KeyboardInputHub::KeyboardInputHub() : impl_{std::make_unique<Impl>()} {}

KeyboardInputHub::~KeyboardInputHub() { stop(); }

bool KeyboardInputHub::isAvailable() noexcept { return isatty(kStdin) == 1; }

core::Status KeyboardInputHub::start() {
    if (impl_->running.load(std::memory_order_acquire)) {
        return core::success();  // Idempotent: buttons and encoder both call this.
    }
    if (!isAvailable()) {
        return core::makeError(core::ErrorCode::DeviceNotConnected, "stdin is not a terminal");
    }

    if (tcgetattr(kStdin, &impl_->originalTermios) != 0) {
        return core::makeError(core::ErrorCode::IoError, "tcgetattr failed");
    }
    impl_->termiosSaved = true;

    termios raw = impl_->originalTermios;
    raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(kStdin, TCSANOW, &raw) != 0) {
        return core::makeError(core::ErrorCode::IoError, "tcsetattr failed");
    }

    impl_->running.store(true, std::memory_order_release);
    impl_->thread = std::thread{[this] { impl_->run(); }};
    return core::success();
}

void KeyboardInputHub::stop() noexcept {
    if (impl_->running.exchange(false, std::memory_order_acq_rel) && impl_->thread.joinable()) {
        impl_->thread.join();
    }
    if (impl_->termiosSaved) {
        tcsetattr(kStdin, TCSANOW, &impl_->originalTermios);
        impl_->termiosSaved = false;
    }
    impl_->buttonListener = nullptr;
    impl_->encoderListener = nullptr;
}

void KeyboardInputHub::setButtonListener(hal::ButtonListener* listener) noexcept {
    impl_->buttonListener = listener;
}

void KeyboardInputHub::setEncoderListener(hal::EncoderListener* listener) noexcept {
    impl_->encoderListener = listener;
}

std::int32_t KeyboardInputHub::consumeDelta() noexcept {
    return impl_->accumulatedDelta.exchange(0, std::memory_order_relaxed);
}

// --- Adapters ---------------------------------------------------------------

KeyboardButtons::KeyboardButtons(std::shared_ptr<KeyboardInputHub> hub) noexcept
    : hub_{std::move(hub)} {}

void KeyboardButtons::setListener(hal::ButtonListener* listener) noexcept {
    hub_->setButtonListener(listener);
}

core::Status KeyboardButtons::start() { return hub_->start(); }

void KeyboardButtons::stop() noexcept { hub_->stop(); }

bool KeyboardButtons::isPressed(core::ButtonId /*id*/) const noexcept { return false; }

KeyboardEncoder::KeyboardEncoder(std::shared_ptr<KeyboardInputHub> hub) noexcept
    : hub_{std::move(hub)} {}

void KeyboardEncoder::setListener(hal::EncoderListener* listener) noexcept {
    hub_->setEncoderListener(listener);
}

core::Status KeyboardEncoder::start() { return hub_->start(); }

void KeyboardEncoder::stop() noexcept { hub_->stop(); }

std::int32_t KeyboardEncoder::consumeDelta() noexcept { return hub_->consumeDelta(); }

}  // namespace aw::platform::desktop
