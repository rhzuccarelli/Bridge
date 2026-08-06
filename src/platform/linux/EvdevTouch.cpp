#include "aw/platform/linux/EvdevTouch.hpp"

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <optional>
#include <thread>

namespace aw::platform::linuxfb {
namespace {

std::uint64_t nowMicros() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

bool hasAbsolutePosition(int descriptor) noexcept {
    // A touch device reports EV_ABS with ABS_X and ABS_Y (or their multitouch
    // equivalents).  Checking the capability bits avoids grabbing a mouse.
    std::array<unsigned long, (ABS_MAX / (8 * sizeof(unsigned long))) + 1> bits{};
    if (ioctl(descriptor, EVIOCGBIT(EV_ABS, static_cast<int>(sizeof(bits))), bits.data()) < 0) {
        return false;
    }
    const auto isSet = [&bits](int code) {
        const std::size_t word = static_cast<std::size_t>(code) / (8 * sizeof(unsigned long));
        const std::size_t bit = static_cast<std::size_t>(code) % (8 * sizeof(unsigned long));
        return (bits[word] >> bit) & 1UL;
    };
    return (isSet(ABS_X) && isSet(ABS_Y)) ||
           (isSet(ABS_MT_POSITION_X) && isSet(ABS_MT_POSITION_Y));
}

struct AxisRange {
    int minimum{0};
    int maximum{4095};

    [[nodiscard]] int scale(int raw, int target) const noexcept {
        const int span = maximum - minimum;
        if (span <= 0) {
            return 0;
        }
        const int clamped = std::clamp(raw, minimum, maximum);
        return ((clamped - minimum) * (target - 1)) / span;
    }
};

}  // namespace

struct EvdevTouch::Impl {
    TouchConfig config;
    std::optional<desktop::VirtualPanelLayout> layout;
    hal::ButtonListener* listener{nullptr};
    int descriptor{-1};
    std::string devicePath;
    AxisRange rangeX;
    AxisRange rangeY;
    std::atomic<bool> running{false};
    std::thread thread;

    // Current contact state.
    int rawX{0};
    int rawY{0};
    bool contact{false};
    std::optional<core::ButtonId> pressedKey;

    void run();
    void handleEvent(const input_event& event);
    void onContactDown();
    void onContactUp();
};

void EvdevTouch::Impl::onContactDown() {
    if (!layout.has_value() || pressedKey.has_value()) {
        return;
    }

    int x = rangeX.scale(rawX, config.panelWidth);
    int y = rangeY.scale(rawY, config.panelHeight);

    if (config.swapAxes) {
        std::swap(x, y);
    }
    if (config.invertX) {
        x = config.panelWidth - 1 - x;
    }
    if (config.invertY) {
        y = config.panelHeight - 1 - y;
    }

    const auto key = layout->hitTest(static_cast<std::int16_t>(x), static_cast<std::int16_t>(y));
    if (!key.has_value()) {
        return;  // A tap on the canvas: ignored, the canvas is not touchable.
    }

    pressedKey = key;
    if (listener != nullptr) {
        listener->onButtonEvent(*key, core::ButtonAction::Pressed, nowMicros());
    }
}

void EvdevTouch::Impl::onContactUp() {
    if (!pressedKey.has_value()) {
        return;
    }
    if (listener != nullptr) {
        listener->onButtonEvent(*pressedKey, core::ButtonAction::Released, nowMicros());
    }
    pressedKey.reset();
}

void EvdevTouch::Impl::handleEvent(const input_event& event) {
    switch (event.type) {
        case EV_ABS:
            if (event.code == ABS_X || event.code == ABS_MT_POSITION_X) {
                rawX = event.value;
            } else if (event.code == ABS_Y || event.code == ABS_MT_POSITION_Y) {
                rawY = event.value;
            }
            return;

        case EV_KEY:
            if (event.code == BTN_TOUCH) {
                contact = event.value != 0;
                if (!contact) {
                    onContactUp();
                }
            }
            return;

        case EV_SYN:
            // Coordinates are only meaningful at a SYN_REPORT boundary; acting
            // on a half-updated position is how you get phantom presses.
            if (event.code == SYN_REPORT && contact) {
                onContactDown();
            }
            return;

        default:
            return;
    }
}

void EvdevTouch::Impl::run() {
    while (running.load(std::memory_order_acquire)) {
        pollfd descriptorSet{descriptor, POLLIN, 0};
        if (::poll(&descriptorSet, 1, 100) <= 0) {
            continue;
        }

        std::array<input_event, 32> events{};
        const ssize_t bytes = ::read(descriptor, events.data(), sizeof(events));
        if (bytes < static_cast<ssize_t>(sizeof(input_event))) {
            continue;
        }

        const std::size_t count = static_cast<std::size_t>(bytes) / sizeof(input_event);
        for (std::size_t i = 0; i < count; ++i) {
            handleEvent(events[i]);
        }
    }
}

EvdevTouch::EvdevTouch(TouchConfig config, desktop::VirtualPanelLayout layout)
    : impl_{std::make_unique<Impl>()} {
    impl_->config = std::move(config);
    impl_->layout = std::move(layout);
}

EvdevTouch::~EvdevTouch() { stop(); }

std::string EvdevTouch::findTouchDevice() {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator{"/dev/input", ec}) {
        const std::string path = entry.path().string();
        if (path.find("/dev/input/event") != 0) {
            continue;
        }
        const int descriptor = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (descriptor < 0) {
            continue;
        }
        const bool suitable = hasAbsolutePosition(descriptor);
        ::close(descriptor);
        if (suitable) {
            return path;
        }
    }
    return {};
}

std::string EvdevTouch::devicePath() const { return impl_->devicePath; }

core::Status EvdevTouch::start() {
    if (impl_->running.load(std::memory_order_acquire)) {
        return core::success();
    }

    impl_->devicePath =
        impl_->config.device.empty() ? findTouchDevice() : impl_->config.device;
    if (impl_->devicePath.empty()) {
        return core::makeError(core::ErrorCode::DeviceNotConnected, "no touch device found");
    }

    impl_->descriptor = ::open(impl_->devicePath.c_str(), O_RDONLY);
    if (impl_->descriptor < 0) {
        return core::makeError(core::ErrorCode::PermissionDenied,
                               impl_->devicePath + " (is the user in 'input'?)");
    }

    // Calibration comes from the kernel driver's reported ranges, so a panel
    // calibrated with the usual userspace tools works here with no extra step.
    input_absinfo absolute{};
    if (ioctl(impl_->descriptor, EVIOCGABS(ABS_X), &absolute) == 0 &&
        absolute.maximum > absolute.minimum) {
        impl_->rangeX = AxisRange{absolute.minimum, absolute.maximum};
    }
    if (ioctl(impl_->descriptor, EVIOCGABS(ABS_Y), &absolute) == 0 &&
        absolute.maximum > absolute.minimum) {
        impl_->rangeY = AxisRange{absolute.minimum, absolute.maximum};
    }

    impl_->running.store(true, std::memory_order_release);
    impl_->thread = std::thread{[this] { impl_->run(); }};
    return core::success();
}

void EvdevTouch::stop() noexcept {
    if (impl_->running.exchange(false, std::memory_order_acq_rel) && impl_->thread.joinable()) {
        impl_->thread.join();
    }
    if (impl_->descriptor >= 0) {
        ::close(impl_->descriptor);
        impl_->descriptor = -1;
    }
    impl_->listener = nullptr;
}

void EvdevTouch::setListener(hal::ButtonListener* listener) noexcept {
    impl_->listener = listener;
}

bool EvdevTouch::isPressed(core::ButtonId id) const noexcept {
    return impl_->pressedKey.has_value() && *impl_->pressedKey == id;
}

}  // namespace aw::platform::linuxfb
