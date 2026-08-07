#include "aw/platform/desktop/TerminalDisplay.hpp"

#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace aw::platform::desktop {
namespace {

constexpr int kStdout = STDOUT_FILENO;

struct Rgb {
    int r;
    int g;
    int b;
};

/// RGB565 -> 8-bit-per-channel, with the low bits replicated so full-scale
/// values reach 255 rather than 248.
constexpr Rgb toRgb888(std::uint16_t colour) noexcept {
    const int r5 = (colour >> 11) & 0x1F;
    const int g6 = (colour >> 5) & 0x3F;
    const int b5 = colour & 0x1F;
    return Rgb{(r5 * 255 + 15) / 31, (g6 * 255 + 31) / 63, (b5 * 255 + 15) / 31};
}

struct TerminalSize {
    int columns{80};
    int rows{24};
};

TerminalSize queryTerminalSize() noexcept {
    winsize size{};
    if (ioctl(kStdout, TIOCGWINSZ, &size) == 0 && size.ws_col > 0 && size.ws_row > 0) {
        return TerminalSize{size.ws_col, size.ws_row};
    }
    return TerminalSize{};
}

}  // namespace

struct TerminalDisplay::Impl {
    std::uint16_t width{320};
    std::uint16_t height{240};
    std::vector<std::uint16_t> pixels;
    std::string frame;  ///< Reused across frames so present() does not allocate.
    bool open{false};
    bool interactive{false};
    int scale{4};
};

TerminalDisplay::TerminalDisplay(std::uint16_t width, std::uint16_t height)
    : impl_{std::make_unique<Impl>()} {
    impl_->width = width;
    impl_->height = height;
}

TerminalDisplay::~TerminalDisplay() { close(); }

bool TerminalDisplay::isAvailable() noexcept { return isatty(kStdout) == 1; }

hal::DisplayInfo TerminalDisplay::info() const noexcept {
    return hal::DisplayInfo{impl_->width, impl_->height, false};
}

core::Status TerminalDisplay::open() {
    impl_->pixels.assign(static_cast<std::size_t>(impl_->width) * impl_->height, 0);
    impl_->interactive = isAvailable();

    if (impl_->interactive) {
        const TerminalSize size = queryTerminalSize();
        // One cell is one pixel wide and two pixels tall, so the vertical budget
        // is rows*2.  Leave one row for the status line the app may print.
        const int horizontal = (impl_->width + size.columns - 1) / std::max(size.columns, 1);
        const int vertical =
            (impl_->height + (size.rows - 1) * 2 - 1) / std::max((size.rows - 1) * 2, 1);
        impl_->scale = std::max({horizontal, vertical, 1});

        // Use the terminal's alternate screen so the preview does not overwrite
        // the user's shell history, and quitting cleanly restores the shell.
        std::fwrite("\x1b[?1049h\x1b[2J\x1b[H\x1b[?25l", 1, 20, stdout);
        std::fflush(stdout);
    }

    impl_->open = true;
    return core::success();
}

void TerminalDisplay::close() noexcept {
    if (!impl_->open) {
        return;
    }
    if (impl_->interactive) {
        // Reset colours, show the cursor, and restore the normal screen buffer.
        std::fwrite("\x1b[0m\x1b[?25h\x1b[?1049l", 1, 20, stdout);
        std::fflush(stdout);
    }
    impl_->pixels.clear();
    impl_->open = false;
}

core::Status TerminalDisplay::blit(const hal::Rect& area,
                                   std::span<const std::uint16_t> pixels) {
    if (!impl_->open) {
        return core::makeError(core::ErrorCode::InvalidState, "display not open");
    }
    if (pixels.size() < static_cast<std::size_t>(area.pixelCount())) {
        return core::makeError(core::ErrorCode::InvalidArgument, "short pixel span");
    }

    for (std::int16_t y = 0; y < area.height; ++y) {
        const int targetRow = area.y + y;
        if (targetRow < 0 || targetRow >= impl_->height) {
            continue;
        }
        for (std::int16_t x = 0; x < area.width; ++x) {
            const int targetColumn = area.x + x;
            if (targetColumn < 0 || targetColumn >= impl_->width) {
                continue;
            }
            impl_->pixels[static_cast<std::size_t>(targetRow) * impl_->width +
                          static_cast<std::size_t>(targetColumn)] =
                pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(area.width) +
                       static_cast<std::size_t>(x)];
        }
    }
    return core::success();
}

core::Status TerminalDisplay::present() {
    if (!impl_->open) {
        return core::makeError(core::ErrorCode::InvalidState, "display not open");
    }
    if (!impl_->interactive) {
        return core::success();  // Redirected output: accept the frame, draw nothing.
    }

    const int scale = impl_->scale;
    const int previewWidth = impl_->width / scale;
    const int previewHeight = impl_->height / scale;

    std::string& out = impl_->frame;
    out.clear();
    out += "\x1b[H";  // Cursor home; no clear, which would flicker.

    const auto sample = [this, scale](int px, int py) -> std::uint16_t {
        const int sx = std::min(px * scale, impl_->width - 1);
        const int sy = std::min(py * scale, impl_->height - 1);
        return impl_->pixels[static_cast<std::size_t>(sy) * impl_->width +
                             static_cast<std::size_t>(sx)];
    };

    char escape[32];
    for (int row = 0; row < previewHeight; row += 2) {
        for (int column = 0; column < previewWidth; ++column) {
            const Rgb top = toRgb888(sample(column, row));
            const Rgb bottom = toRgb888(sample(column, std::min(row + 1, previewHeight - 1)));

            const int written =
                std::snprintf(escape, sizeof(escape), "\x1b[38;2;%d;%d;%dm\x1b[48;2;%d;%d;%dm",
                              top.r, top.g, top.b, bottom.r, bottom.g, bottom.b);
            out.append(escape, static_cast<std::size_t>(written));
            out += "▀";  // Upper half block.
        }
        out += "\x1b[0m\n";
    }

    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
    return core::success();
}

core::Status TerminalDisplay::setBacklight(float /*level*/) {
    return core::notImplemented("TerminalDisplay has no backlight");
}

}  // namespace aw::platform::desktop
