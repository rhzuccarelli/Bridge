#include "aw/platform/oled/Ssd1306Display.hpp"

#include <algorithm>
#include <array>

namespace aw::platform::oled {
namespace {

// Command bytes, named so the init sequence reads as intent rather than magic.
constexpr std::uint8_t kSetContrast = 0x81;
constexpr std::uint8_t kDisplayAllOnResume = 0xA4;
constexpr std::uint8_t kNormalDisplay = 0xA6;
constexpr std::uint8_t kDisplayOff = 0xAE;
constexpr std::uint8_t kDisplayOn = 0xAF;
constexpr std::uint8_t kSetDisplayOffset = 0xD3;
constexpr std::uint8_t kSetComPins = 0xDA;
constexpr std::uint8_t kSetVcomDetect = 0xDB;
constexpr std::uint8_t kSetDisplayClockDiv = 0xD5;
constexpr std::uint8_t kSetPrecharge = 0xD9;
constexpr std::uint8_t kSetMultiplex = 0xA8;
constexpr std::uint8_t kSetStartLine = 0x40;
constexpr std::uint8_t kMemoryMode = 0x20;       // SSD1306 only.
constexpr std::uint8_t kChargePumpSsd = 0x8D;    // SSD1306.
constexpr std::uint8_t kChargePumpSh = 0xAD;     // SH1106.
constexpr std::uint8_t kSegRemapNormal = 0xA1;   // column 127 -> SEG0.
constexpr std::uint8_t kSegRemapFlipped = 0xA0;
constexpr std::uint8_t kComScanDec = 0xC8;       // top-to-bottom.
constexpr std::uint8_t kComScanInc = 0xC0;
constexpr std::uint8_t kSetPageStart = 0xB0;

}  // namespace

Ssd1306Display::Ssd1306Display(OledConfig config, std::unique_ptr<OledBus> bus)
    : config_{config}, bus_{std::move(bus)} {
    buffer_.assign(static_cast<std::size_t>(config_.width) * (config_.height / 8), 0);
}

Ssd1306Display::~Ssd1306Display() { close(); }

hal::DisplayInfo Ssd1306Display::info() const noexcept {
    // supportsBacklightControl is true: an OLED has no backlight, but we map the
    // call onto the contrast register, so the capability is honestly present.
    return hal::DisplayInfo{config_.width, config_.height, true};
}

bool Ssd1306Display::isLit(std::uint16_t rgb565) noexcept {
    // Collapse colour to luminance and threshold at the midpoint. The UI paints
    // bright text/accents on a near-black background, so this maps cleanly:
    // background off, everything meaningful on.
    const int r = (rgb565 >> 11) & 0x1F;
    const int g = (rgb565 >> 5) & 0x3F;
    const int b = rgb565 & 0x1F;
    // Weights approximate Rec. 601 luma, scaled so a fully-lit pixel is ~255.
    const int luma = (r * 77) / 31 + (g * 150) / 63 + (b * 28) / 31;
    return luma >= 128;
}

void Ssd1306Display::setPixel(std::int16_t x, std::int16_t y, bool on) noexcept {
    if (x < 0 || y < 0 || x >= static_cast<std::int16_t>(config_.width) ||
        y >= static_cast<std::int16_t>(config_.height)) {
        return;
    }
    const std::size_t index =
        static_cast<std::size_t>(y / 8) * config_.width + static_cast<std::size_t>(x);
    const std::uint8_t mask = static_cast<std::uint8_t>(1U << (y % 8));
    if (on) {
        buffer_[index] |= mask;
    } else {
        buffer_[index] &= static_cast<std::uint8_t>(~mask);
    }
    dirty_ = true;  // Any change to the buffer means the next present() must push.
}

bool Ssd1306Display::pixel(std::int16_t x, std::int16_t y) const noexcept {
    if (x < 0 || y < 0 || x >= static_cast<std::int16_t>(config_.width) ||
        y >= static_cast<std::int16_t>(config_.height)) {
        return false;
    }
    const std::size_t index =
        static_cast<std::size_t>(y / 8) * config_.width + static_cast<std::size_t>(x);
    return (buffer_[index] & (1U << (y % 8))) != 0;
}

std::vector<std::uint8_t> Ssd1306Display::buildInitSequence() const {
    const bool sh1106 = config_.controller == Controller::Sh1106;
    const std::uint8_t segRemap =
        config_.rotation == Rotation::Normal ? kSegRemapNormal : kSegRemapFlipped;
    const std::uint8_t comScan =
        config_.rotation == Rotation::Normal ? kComScanDec : kComScanInc;
    const auto multiplex = static_cast<std::uint8_t>(config_.height - 1);
    // 128x32 panels wire their COM pins sequentially (0x02); 128x64 alternate (0x12).
    const std::uint8_t comPins = config_.height == 32 ? 0x02 : 0x12;

    std::vector<std::uint8_t> seq{
        kDisplayOff,
        kSetDisplayClockDiv, 0x80,
        kSetMultiplex, multiplex,
        kSetDisplayOffset, 0x00,
        static_cast<std::uint8_t>(kSetStartLine | 0x00),
    };

    if (sh1106) {
        seq.insert(seq.end(), {kChargePumpSh, 0x8B});  // Internal DC-DC on.
    } else {
        seq.insert(seq.end(), {kChargePumpSsd, 0x14});          // Charge pump on.
        seq.insert(seq.end(), {kMemoryMode, 0x02});             // Page addressing.
    }

    seq.insert(seq.end(), {
        segRemap,
        comScan,
        kSetComPins, comPins,
        kSetContrast, config_.contrast,
        kSetPrecharge, static_cast<std::uint8_t>(sh1106 ? 0x22 : 0xF1),
        kSetVcomDetect, static_cast<std::uint8_t>(sh1106 ? 0x35 : 0x40),
        kDisplayAllOnResume,
        kNormalDisplay,
        kDisplayOn,
    });
    return seq;
}

core::Status Ssd1306Display::open() {
    if (auto status = bus_->reset(); !status) {
        return status;
    }
    const std::vector<std::uint8_t> init = buildInitSequence();
    if (auto status = bus_->writeCommands(init); !status) {
        return status;
    }
    std::fill(buffer_.begin(), buffer_.end(), std::uint8_t{0});
    open_ = true;
    dirty_ = true;
    return present();
}

void Ssd1306Display::close() noexcept {
    if (open_ && bus_) {
        const std::array<std::uint8_t, 1> off{kDisplayOff};
        (void)bus_->writeCommands(off);
    }
    open_ = false;
}

core::Status Ssd1306Display::blit(const hal::Rect& area,
                                  std::span<const std::uint16_t> pixels) {
    if (pixels.size() < static_cast<std::size_t>(area.pixelCount())) {
        return core::makeError(core::ErrorCode::InvalidArgument, "short pixel span");
    }
    for (std::int16_t row = 0; row < area.height; ++row) {
        for (std::int16_t col = 0; col < area.width; ++col) {
            const std::uint16_t rgb =
                pixels[static_cast<std::size_t>(row) * static_cast<std::size_t>(area.width) +
                       static_cast<std::size_t>(col)];
            setPixel(static_cast<std::int16_t>(area.x + col),
                     static_cast<std::int16_t>(area.y + row), isLit(rgb));
        }
    }
    dirty_ = true;
    return core::success();
}

core::Status Ssd1306Display::present() {
    if (!open_) {
        return core::makeError(core::ErrorCode::InvalidState, "display not open");
    }
    if (!dirty_) {
        return core::success();
    }

    // Page addressing, one page at a time. This path works identically on the
    // SSD1306 and the SH1106 (the latter has no horizontal-addressing mode), so
    // there is one flush routine rather than two. The whole buffer is 1 KiB, so
    // there is no value in tracking damage - a full flush is already sub-millisecond.
    const auto pages = static_cast<std::uint8_t>(config_.height / 8);
    const auto offset = static_cast<std::uint8_t>(config_.effectiveColumnOffset());

    for (std::uint8_t page = 0; page < pages; ++page) {
        const std::array<std::uint8_t, 3> setPage{
            static_cast<std::uint8_t>(kSetPageStart | page),
            static_cast<std::uint8_t>(0x00 | (offset & 0x0F)),         // lower column
            static_cast<std::uint8_t>(0x10 | ((offset >> 4) & 0x0F)),  // higher column
        };
        if (auto status = bus_->writeCommands(setPage); !status) {
            return status;
        }
        const std::size_t start = static_cast<std::size_t>(page) * config_.width;
        if (auto status = bus_->writeData(
                std::span<const std::uint8_t>{buffer_.data() + start, config_.width});
            !status) {
            return status;
        }
    }
    dirty_ = false;
    return core::success();
}

core::Status Ssd1306Display::setBacklight(float level) {
    const float clamped = std::clamp(level, 0.0F, 1.0F);
    const auto value = static_cast<std::uint8_t>(clamped * 255.0F);
    const std::array<std::uint8_t, 2> command{kSetContrast, value};
    if (!open_) {
        config_.contrast = value;  // Applied at the next open().
        return core::success();
    }
    return bus_->writeCommands(command);
}

core::Status Ssd1306Display::showTestPattern() {
    if (!open_) {
        return core::makeError(core::ErrorCode::InvalidState, "display not open");
    }
    std::fill(buffer_.begin(), buffer_.end(), std::uint8_t{0});

    const auto w = static_cast<std::int16_t>(config_.width);
    const auto h = static_cast<std::int16_t>(config_.height);

    // A 1px border proves every edge column/row addresses correctly.
    for (std::int16_t x = 0; x < w; ++x) {
        setPixel(x, 0, true);
        setPixel(x, static_cast<std::int16_t>(h - 1), true);
    }
    for (std::int16_t y = 0; y < h; ++y) {
        setPixel(0, y, true);
        setPixel(static_cast<std::int16_t>(w - 1), y, true);
    }
    // A diagonal shows orientation (top-left to bottom-right).
    for (std::int16_t i = 0; i < std::min(w, h); ++i) {
        setPixel(i, static_cast<std::int16_t>(i * h / w), true);
    }
    // A filled 8x8 block in the top-left marks the origin unambiguously.
    for (std::int16_t y = 2; y < 10; ++y) {
        for (std::int16_t x = 2; x < 10; ++x) {
            setPixel(x, y, true);
        }
    }
    dirty_ = true;
    return present();
}

}  // namespace aw::platform::oled
