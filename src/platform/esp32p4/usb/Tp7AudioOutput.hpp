#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "EspUsbStack.hpp"
#include "aw/hal/AudioOutputInterface.hpp"
#include "aw/platform/Tp7Monitor.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace aw::platform::esp32p4::usb {

/// Monitor audio through a TP-7 in USB-audio mode (port step P3).
///
/// The engine stays at 48 kHz stereo. The TP-7's playback endpoint takes only
/// 96 kHz, six channels of 24-bit in 3 bytes, over a high-bandwidth
/// isochronous endpoint (2 x 882 bytes per microframe, one packet per 1 ms),
/// so this backend converts at the boundary as `AlsaAudioOutput` does on the
/// Pi (`convertStereo48ToTp7Monitor96`, FL/FR carry the mix).
///
/// Two sides meet in a `FrameFifo` of packed 96 kHz frames:
///  - a render task calls the engine for one period whenever the FIFO runs
///    below its target, converts and packs it;
///  - the USB client task, at each playback completion, refills that transfer
///    with as many frames per packet as the feedback endpoint asks for
///    (`IsoPacketSizer`) and resubmits it.
/// The render task refills by FIFO level, so the engine runs on the TP-7's
/// clock. A FIFO that runs dry plays silence and counts an xrun.
///
/// ESP-IDF v5.5 needs help with this endpoint (see docs/esp32p4-port.md, P3a):
/// SET_INTERFACE is sent here, and the host channel's multi-count and data
/// PID are written directly, as the probe proved on the bench.
///
/// When its host disappears mid-stream (the P4 reset or lost power while the
/// TP-7 stayed on), the TP-7 answers the feedback endpoint with empty packets
/// from then on, through bus resets and alternate-setting changes, until it
/// re-enumerates. Open-loop at a nominal 96 kHz its buffer drifts to a limit
/// within seconds and it slips samples. So when `refresh()` sees no rate for
/// 1.5 s it calls the device-reset hook, which sends the TP-7 through MTP
/// mode and back; `FallbackAudioOutput` covers the gap and reopens.
class Tp7AudioOutput final : public hal::AudioOutputInterface {
public:
    explicit Tp7AudioOutput(std::shared_ptr<EspUsbStack> stack);
    ~Tp7AudioOutput() override;

    core::Status open(const core::AudioFormat& format, core::FrameCount bufferFrames,
                      hal::AudioRenderCallback& callback) override;
    core::Status start() override;
    core::Status refresh() override;
    void stop() noexcept override;
    void close() noexcept override;

    [[nodiscard]] bool isRunning() const noexcept override;

    /// Makes the TP-7 re-enumerate; must not block (it runs from `refresh()`).
    void setDeviceResetHook(std::function<void()> hook) { resetHook_ = std::move(hook); }
    [[nodiscard]] core::AudioFormat format() const noexcept override { return format_; }
    [[nodiscard]] core::FrameCount bufferFrames() const noexcept override { return periodFrames_; }
    [[nodiscard]] core::FrameCount latencyFrames() const noexcept override;

    /// What the stream has done since `open`, for logs and bench tests.
    struct Stats {
        std::uint32_t packets{0};
        std::uint64_t frames{0};          ///< 96 kHz frames sent.
        std::uint32_t underruns{0};       ///< Packets padded with silence.
        std::uint32_t failedPackets{0};
        std::uint32_t feedbackReports{0};
        std::uint32_t emptyFeedback{0};   ///< Feedback packets that carried no value.
        std::uint32_t framesPerMsQ16{0};  ///< Current rate.
        std::uint32_t resets{0};          ///< Device resets asked by the feedback watchdog.
        std::uint32_t fifoFrames{0};
    };
    [[nodiscard]] Stats stats() const noexcept;

private:
    static void renderTask(void* context);
    static void playbackDone(usb_transfer_t* transfer);
    static void feedbackDone(usb_transfer_t* transfer);

    core::Status configureDevice();
    void startStream();
    void stopStream() noexcept;
    void fillPacket(usb_transfer_t* transfer) noexcept;
    void releaseDevice() noexcept;

    std::shared_ptr<EspUsbStack> stack_;
    std::optional<Lease> lease_;
    bool claimed_{false};
    Layout::Playback playback_;

    core::AudioFormat format_{core::kNativeFormat};
    core::FrameCount periodFrames_{256};
    hal::AudioRenderCallback* callback_{nullptr};
    std::vector<float> stereo_;
    std::vector<float> wide_;
    std::vector<std::uint8_t> packed_;
    Tp7MonitorConverterState converter_;
    std::unique_ptr<FrameFifo> fifo_;
    IsoPacketSizer sizer_{kTp7MonitorRate};

    std::vector<usb_transfer_t*> transfers_;
    std::vector<usb_transfer_t*> feedbacks_;
    std::atomic<bool> streaming_{false};   ///< Transfers resubmit while set.
    std::atomic<int> inFlight_{0};
    std::atomic<bool> deviceLost_{false};

    TaskHandle_t renderTask_{nullptr};
    SemaphoreHandle_t renderExited_{nullptr};
    std::atomic<bool> rendering_{false};
    std::atomic<bool> running_{false};

    std::atomic<std::uint32_t> packets_{0};
    std::atomic<std::uint64_t> frames_{0};
    std::atomic<std::uint32_t> underruns_{0};
    std::atomic<std::uint32_t> failedPackets_{0};
    std::atomic<std::uint32_t> feedbackReports_{0};
    std::atomic<std::uint32_t> emptyFeedback_{0};
    std::atomic<std::uint32_t> resets_{0};
    std::function<void()> resetHook_;
    std::uint32_t consecutiveResets_{0};  ///< Since a rate last arrived; survives reopening.
    bool resetRequested_{false};          ///< For this open.
    std::uint32_t reportedUnderruns_{0};  ///< Render task: xruns already passed up.

    std::int64_t streamStartedUs_{0};
    std::uint32_t feedbackAtCheck_{0};
    std::int64_t feedbackCheckedUs_{0};
    bool loggedNoFeedback_{false};
};

}  // namespace aw::platform::esp32p4::usb
