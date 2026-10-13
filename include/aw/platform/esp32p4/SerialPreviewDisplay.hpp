#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "aw/hal/DisplayInterface.hpp"
#include "aw/platform/oled/Ssd1306Display.hpp"

namespace aw::platform::esp32p4 {

/// The SSD1306 display as the device will drive it, previewed on the serial log.
///
/// The UI's frames go through the real `oled::Ssd1306Display` (same
/// thresholding, same page packing), over a bus that discards the bytes. After
/// each `present()`, the packed page buffer is what the OLED would show; when
/// it changes, a background thread prints it as text, two pixel rows per line
/// using Unicode half blocks.
///
/// Printing runs on its own thread because a frame takes about a second of UART
/// time and the UI loop must keep its 30 Hz. Frames that arrive while one is
/// printing are coalesced: only the latest is printed, and frames are at least
/// `intervalMillis` apart. The last frame is always printed eventually.
class SerialPreviewDisplay final : public hal::DisplayInterface {
public:
    explicit SerialPreviewDisplay(std::uint32_t intervalMillis);
    ~SerialPreviewDisplay() override;

    [[nodiscard]] hal::DisplayInfo info() const noexcept override;

    core::Status open() override;
    void close() noexcept override;
    core::Status blit(const hal::Rect& area, std::span<const std::uint16_t> pixels) override;
    core::Status present() override;
    core::Status setBacklight(float level) override;

private:
    void printLoop();
    void print(const std::vector<std::uint8_t>& pages, std::uint32_t frame);

    oled::Ssd1306Display panel_;
    std::uint32_t intervalMillis_;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<std::uint8_t> pending_;  ///< Latest page buffer, guarded by mutex_.
    std::uint32_t pendingFrame_{0};      ///< Bumped on each change; 0 = nothing yet.
    bool stopping_{false};
    std::thread printer_;
};

}  // namespace aw::platform::esp32p4
