#include "aw/platform/esp32p4/SerialPreviewDisplay.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>

namespace aw::platform::esp32p4 {
namespace {

/// The panel's bytes go nowhere: the page buffer, not the wire, is what gets
/// previewed. Replaced by an `esp_lcd`/SPI bus in step P2.
class DiscardingOledBus final : public oled::OledBus {
public:
    core::Status reset() override { return core::success(); }
    core::Status writeCommands(std::span<const std::uint8_t> /*commands*/) override {
        return core::success();
    }
    core::Status writeData(std::span<const std::uint8_t> /*data*/) override {
        return core::success();
    }
};

constexpr const char* kUpperHalf = "▀";
constexpr const char* kLowerHalf = "▄";
constexpr const char* kFullBlock = "█";

}  // namespace

SerialPreviewDisplay::SerialPreviewDisplay(std::uint32_t intervalMillis)
    : panel_{oled::OledConfig{}, std::make_unique<DiscardingOledBus>()},
      intervalMillis_{intervalMillis} {}

SerialPreviewDisplay::~SerialPreviewDisplay() { close(); }

hal::DisplayInfo SerialPreviewDisplay::info() const noexcept { return panel_.info(); }

core::Status SerialPreviewDisplay::open() {
    if (auto status = panel_.open(); !status) return status;
    {
        std::lock_guard lock{mutex_};
        stopping_ = false;
        pending_.clear();
        pendingFrame_ = 0;
    }
    printer_ = std::thread{[this] { printLoop(); }};
    return core::success();
}

void SerialPreviewDisplay::close() noexcept {
    {
        std::lock_guard lock{mutex_};
        stopping_ = true;
    }
    wake_.notify_one();
    if (printer_.joinable()) printer_.join();
    panel_.close();
}

core::Status SerialPreviewDisplay::blit(const hal::Rect& area,
                                        std::span<const std::uint16_t> pixels) {
    return panel_.blit(area, pixels);
}

core::Status SerialPreviewDisplay::present() {
    if (auto status = panel_.present(); !status) return status;
    const auto pages = panel_.pageBuffer();
    {
        std::lock_guard lock{mutex_};
        if (std::equal(pages.begin(), pages.end(), pending_.begin(), pending_.end())) {
            return core::success();
        }
        pending_.assign(pages.begin(), pages.end());
        ++pendingFrame_;
    }
    wake_.notify_one();
    return core::success();
}

core::Status SerialPreviewDisplay::setBacklight(float level) {
    return panel_.setBacklight(level);
}

void SerialPreviewDisplay::printLoop() {
    std::uint32_t printedFrame = 0;
    std::vector<std::uint8_t> pages;
    for (;;) {
        std::uint32_t frame = 0;
        {
            std::unique_lock lock{mutex_};
            wake_.wait(lock, [&] { return stopping_ || pendingFrame_ != printedFrame; });
            if (stopping_) return;
            pages = pending_;
            frame = pendingFrame_;
        }
        print(pages, frame);
        printedFrame = frame;

        std::unique_lock lock{mutex_};
        wake_.wait_for(lock, std::chrono::milliseconds{intervalMillis_},
                       [this] { return stopping_; });
        if (stopping_) return;
    }
}

void SerialPreviewDisplay::print(const std::vector<std::uint8_t>& pages, std::uint32_t frame) {
    const hal::DisplayInfo size = panel_.info();
    // Reads the snapshot, not the live panel, which the UI thread keeps drawing into.
    const auto lit = [&](int x, int y) {
        const std::size_t index = static_cast<std::size_t>(y / 8) * size.width +
                                  static_cast<std::size_t>(x);
        return index < pages.size() && ((pages[index] >> (y % 8)) & 1U) != 0;
    };

    std::string text;
    text.reserve(static_cast<std::size_t>(size.width + 4) * (size.height / 2 + 3) * 3);
    text += "[display] frame " + std::to_string(frame) + "\n+";
    text.append(size.width, '-');
    text += "+\n";
    for (int y = 0; y < size.height; y += 2) {
        text += '|';
        for (int x = 0; x < size.width; ++x) {
            const bool top = lit(x, y);
            const bool bottom = lit(x, y + 1);
            text += top && bottom ? kFullBlock : top ? kUpperHalf : bottom ? kLowerHalf : " ";
        }
        text += "|\n";
    }
    text += '+';
    text.append(size.width, '-');
    text += "+\n";
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

}  // namespace aw::platform::esp32p4
