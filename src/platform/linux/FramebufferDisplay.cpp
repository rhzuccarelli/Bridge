#include "aw/platform/linux/FramebufferDisplay.hpp"

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <optional>

namespace aw::platform::linuxfb {
namespace {

/// RGB565 -> the framebuffer's 32 bpp layout, honouring the offsets the kernel
/// reports rather than assuming BGRA.  Different Pi firmware revisions disagree
/// about channel order, and guessing produces a display with red and blue
/// swapped - a bug that wastes an afternoon.
struct Bgra32 {
    std::uint32_t redOffset{16};
    std::uint32_t greenOffset{8};
    std::uint32_t blueOffset{0};

    [[nodiscard]] std::uint32_t pack(std::uint16_t colour) const noexcept {
        const std::uint32_t r = ((colour >> 11) & 0x1FU) * 255U / 31U;
        const std::uint32_t g = ((colour >> 5) & 0x3FU) * 255U / 63U;
        const std::uint32_t b = (colour & 0x1FU) * 255U / 31U;
        return (r << redOffset) | (g << greenOffset) | (b << blueOffset);
    }
};

}  // namespace

struct FramebufferDisplay::Impl {
    FramebufferConfig config;
    int descriptor{-1};
    unsigned char* memory{nullptr};
    std::size_t mappedBytes{0};
    std::uint32_t lineLengthBytes{0};
    std::uint16_t panelWidth{0};
    std::uint16_t panelHeight{0};
    std::uint16_t bitsPerPixel{0};
    Bgra32 packer;
    std::optional<desktop::VirtualPanelLayout> presetLayout;
    std::optional<desktop::VirtualPanelLayout> layout;

    void writePixel(int x, int y, std::uint16_t colour) noexcept {
        if (x < 0 || y < 0 || x >= panelWidth || y >= panelHeight) {
            return;
        }
        const std::size_t rowOffset = static_cast<std::size_t>(y) * lineLengthBytes;
        if (bitsPerPixel == 16) {
            auto* target = reinterpret_cast<std::uint16_t*>(memory + rowOffset);
            target[x] = colour;
        } else {
            auto* target = reinterpret_cast<std::uint32_t*>(memory + rowOffset);
            target[x] = packer.pack(colour);
        }
    }

    void fillRect(const hal::Rect& area, std::uint16_t colour) noexcept {
        for (std::int16_t y = 0; y < area.height; ++y) {
            for (std::int16_t x = 0; x < area.width; ++x) {
                writePixel(area.x + x, area.y + y, colour);
            }
        }
    }

    /// Draws the stand-in control panel once, at open().  Deliberately crude -
    /// outlined boxes, no text (the bitmap font is Phase 7) - because its job is
    /// to show where to press, not to be part of the product's visual design.
    void drawVirtualPanel() noexcept {
        if (!layout.has_value() || layout->keyStripArea().isEmpty()) {
            return;
        }

        constexpr std::uint16_t kStripBackground = 0x1082;  // Near black.
        constexpr std::uint16_t kKeyFill = 0x2945;          // Dark grey.
        constexpr std::uint16_t kKeyBorder = 0xFC00;        // Amber, matching the UI accent.

        fillRect(layout->keyStripArea(), kStripBackground);

        for (const desktop::VirtualKey& key : layout->keys()) {
            const hal::Rect inner{static_cast<std::int16_t>(key.area.x + 2),
                                  static_cast<std::int16_t>(key.area.y + 2),
                                  static_cast<std::int16_t>(key.area.width - 4),
                                  static_cast<std::int16_t>(key.area.height - 4)};
            fillRect(inner, kKeyFill);
            fillRect(hal::Rect{inner.x, inner.y, inner.width, 1}, kKeyBorder);
            fillRect(hal::Rect{inner.x, static_cast<std::int16_t>(inner.y + inner.height - 1),
                               inner.width, 1},
                     kKeyBorder);
            fillRect(hal::Rect{inner.x, inner.y, 1, inner.height}, kKeyBorder);
            fillRect(hal::Rect{static_cast<std::int16_t>(inner.x + inner.width - 1), inner.y, 1,
                               inner.height},
                     kKeyBorder);
        }
    }
};

FramebufferDisplay::FramebufferDisplay(FramebufferConfig config,
                                       std::optional<desktop::VirtualPanelLayout> layout)
    : impl_{std::make_unique<Impl>()} {
    impl_->config = std::move(config);
    impl_->presetLayout = std::move(layout);
}

FramebufferDisplay::~FramebufferDisplay() { close(); }

bool FramebufferDisplay::isAvailable(const std::string& device) noexcept {
    return ::access(device.c_str(), R_OK | W_OK) == 0;
}

std::optional<FramebufferDisplay::PanelSize> FramebufferDisplay::queryPanelSize(
    const std::string& device) noexcept {
    const int descriptor = ::open(device.c_str(), O_RDONLY);
    if (descriptor < 0) {
        return std::nullopt;
    }
    fb_var_screeninfo variable{};
    const bool ok = ioctl(descriptor, FBIOGET_VSCREENINFO, &variable) == 0;
    ::close(descriptor);
    if (!ok) {
        return std::nullopt;
    }
    return PanelSize{static_cast<std::uint16_t>(variable.xres),
                     static_cast<std::uint16_t>(variable.yres)};
}

hal::DisplayInfo FramebufferDisplay::info() const noexcept {
    return hal::DisplayInfo{impl_->config.canvasWidth, impl_->config.canvasHeight, false};
}

std::uint16_t FramebufferDisplay::panelWidth() const noexcept { return impl_->panelWidth; }
std::uint16_t FramebufferDisplay::panelHeight() const noexcept { return impl_->panelHeight; }

const desktop::VirtualPanelLayout* FramebufferDisplay::panelLayout() const noexcept {
    return impl_->layout.has_value() ? &*impl_->layout : nullptr;
}

core::Status FramebufferDisplay::open() {
    impl_->descriptor = ::open(impl_->config.device.c_str(), O_RDWR);
    if (impl_->descriptor < 0) {
        return core::makeError(core::ErrorCode::NotFound,
                               impl_->config.device + ": cannot open (is the user in 'video'?)");
    }

    fb_var_screeninfo variable{};
    fb_fix_screeninfo fixed{};
    if (ioctl(impl_->descriptor, FBIOGET_VSCREENINFO, &variable) != 0 ||
        ioctl(impl_->descriptor, FBIOGET_FSCREENINFO, &fixed) != 0) {
        close();
        return core::makeError(core::ErrorCode::IoError, "FBIOGET_*SCREENINFO failed");
    }

    if (variable.bits_per_pixel != 16 && variable.bits_per_pixel != 32) {
        close();
        return core::makeError(
            core::ErrorCode::UnsupportedFormat,
            std::to_string(variable.bits_per_pixel) + " bpp framebuffer (need 16 or 32)");
    }

    impl_->panelWidth = static_cast<std::uint16_t>(variable.xres);
    impl_->panelHeight = static_cast<std::uint16_t>(variable.yres);
    impl_->bitsPerPixel = static_cast<std::uint16_t>(variable.bits_per_pixel);
    impl_->lineLengthBytes = fixed.line_length;
    impl_->packer = Bgra32{variable.red.offset, variable.green.offset, variable.blue.offset};
    impl_->mappedBytes = static_cast<std::size_t>(fixed.line_length) * variable.yres;

    void* mapping = ::mmap(nullptr, impl_->mappedBytes, PROT_READ | PROT_WRITE, MAP_SHARED,
                           impl_->descriptor, 0);
    if (mapping == MAP_FAILED) {
        close();
        return core::makeError(core::ErrorCode::IoError, "mmap of the framebuffer failed");
    }
    impl_->memory = static_cast<unsigned char*>(mapping);

    if (impl_->config.stretch) {
        impl_->config.canvasWidth = impl_->panelWidth;
        impl_->config.canvasHeight = impl_->panelHeight;
    }

    if (impl_->presetLayout.has_value()) {
        impl_->layout = impl_->presetLayout;  // Shared with the touch backend.
    } else {
        impl_->layout.emplace(impl_->panelWidth, impl_->panelHeight, impl_->config.canvasWidth,
                              impl_->config.canvasHeight);
    }

    // Clear the whole panel so nothing of the console shows through the border.
    std::memset(impl_->memory, 0, impl_->mappedBytes);
    if (impl_->config.drawVirtualPanel) {
        impl_->drawVirtualPanel();
    }

    return core::success();
}

void FramebufferDisplay::close() noexcept {
    if (impl_->memory != nullptr) {
        ::munmap(impl_->memory, impl_->mappedBytes);
        impl_->memory = nullptr;
    }
    if (impl_->descriptor >= 0) {
        ::close(impl_->descriptor);
        impl_->descriptor = -1;
    }
    impl_->layout.reset();
}

core::Status FramebufferDisplay::blit(const hal::Rect& area,
                                      std::span<const std::uint16_t> pixels) {
    if (impl_->memory == nullptr) {
        return core::makeError(core::ErrorCode::InvalidState, "framebuffer not open");
    }
    if (pixels.size() < static_cast<std::size_t>(area.pixelCount())) {
        return core::makeError(core::ErrorCode::InvalidArgument, "short pixel span");
    }

    const hal::Rect canvas = impl_->layout->canvasArea();

    for (std::int16_t y = 0; y < area.height; ++y) {
        const std::size_t sourceRow =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(area.width);
        for (std::int16_t x = 0; x < area.width; ++x) {
            impl_->writePixel(canvas.x + area.x + x, canvas.y + area.y + y,
                              pixels[sourceRow + static_cast<std::size_t>(x)]);
        }
    }
    return core::success();
}

core::Status FramebufferDisplay::present() {
    // Writes go straight into the mapping, so there is nothing to flip.  Kept as
    // a no-op rather than removed: a double-buffered panel would need it, and
    // callers must not learn to skip it.
    return core::success();
}

core::Status FramebufferDisplay::setBacklight(float /*level*/) {
    // An HDMI panel's backlight is not ours to control.
    return core::notImplemented("HDMI panel backlight is not host controlled");
}

}  // namespace aw::platform::linuxfb
