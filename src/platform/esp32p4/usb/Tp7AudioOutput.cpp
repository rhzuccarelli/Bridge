#include "Tp7AudioOutput.hpp"

#include <algorithm>
#include <cstring>
#include <span>
#include <string>

#include "BulkPipe.hpp"
#include "aw/core/Logger.hpp"
#include "esp_timer.h"
#include "soc/usb_dwc_struct.h"

namespace aw::platform::esp32p4::usb {
namespace {

constexpr std::string_view kModule = "tp7-audio";
constexpr std::uint16_t kTeenageEngineering = 0x2367;

/// Playback transfers kept in flight, and 1 ms packets in each: the probe's
/// proven setting. A completion arrives every 4 ms with 8 ms still queued.
constexpr int kTransfers = 3;
constexpr int kPacketsPerTransfer = 4;
constexpr int kFeedbackTransfers = 3;
constexpr int kFeedbackPackets = 4;
/// ESP-IDF keeps 64 isochronous descriptors per transfer, 3 of them margin.
constexpr int kUsableIsocDescriptors = 64 - 3;

/// The render task tops the FIFO up whenever it holds less than this many
/// 96 kHz frames (about 10.7 ms), one period at a time.
constexpr std::size_t kTargetFrames = 1024;
constexpr std::size_t kFifoFrames = 4096;

/// The feedback watchdog: reset the TP-7 after this long without a rate, at
/// most this many times in a row.
constexpr std::int64_t kFeedbackTimeoutUs = 1'500'000;
constexpr std::uint32_t kMaxResets = 2;

/// HCCHAR.EPType of an isochronous channel, and HCTSIZ.PID encodings.
constexpr std::uint32_t kDwcIsochronous = 1;
constexpr std::uint32_t kPidData0 = 0;
constexpr std::uint32_t kPidData2 = 1;
constexpr std::uint32_t kPidData1 = 2;
constexpr std::uint32_t kPidMData = 3;

/// One control transfer on EP0, waited for. `data` is sent for an OUT request
/// and filled for an IN request. Never call this on the USB client task.
esp_err_t controlTransfer(usb_host_client_handle_t client, usb_device_handle_t device,
                          const usb_setup_packet_t& setup, std::uint8_t* data) {
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (done == nullptr) return ESP_ERR_NO_MEM;
    usb_transfer_t* transfer = nullptr;
    esp_err_t err = usb_host_transfer_alloc(
        USB_SETUP_PACKET_SIZE + std::max<std::size_t>(64, setup.wLength), 0, &transfer);
    if (err == ESP_OK) {
        std::memcpy(transfer->data_buffer, &setup, USB_SETUP_PACKET_SIZE);
        const bool isIn = (setup.bmRequestType & USB_BM_REQUEST_TYPE_DIR_IN) != 0;
        if (!isIn && setup.wLength > 0)
            std::memcpy(transfer->data_buffer + USB_SETUP_PACKET_SIZE, data, setup.wLength);
        transfer->num_bytes = USB_SETUP_PACKET_SIZE + setup.wLength;
        transfer->device_handle = device;
        transfer->bEndpointAddress = 0;
        transfer->callback = [](usb_transfer_t* t) {
            xSemaphoreGive(static_cast<SemaphoreHandle_t>(t->context));
        };
        transfer->context = done;
        err = usb_host_transfer_submit_control(client, transfer);
        if (err == ESP_OK) {
            if (xSemaphoreTake(done, pdMS_TO_TICKS(1000)) != pdTRUE) {
                // The transfer still owns the semaphore; leak both rather than
                // free them under a pending completion.
                return ESP_ERR_TIMEOUT;
            }
            if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
                err = ESP_FAIL;
            } else if (isIn && setup.wLength > 0) {
                std::memcpy(data, transfer->data_buffer + USB_SETUP_PACKET_SIZE, setup.wLength);
            }
        }
        usb_host_transfer_free(transfer);
    }
    vSemaphoreDelete(done);
    return err;
}

/// `usb_host_interface_claim` only allocates the host's pipes; it never sends
/// SET_INTERFACE. Without it the TP-7 stays on alternate 0, ignores the audio
/// and never answers the feedback endpoint.
esp_err_t setInterface(usb_host_client_handle_t client, usb_device_handle_t device,
                       std::uint8_t interface, std::uint8_t alternate) {
    usb_setup_packet_t setup{};
    USB_SETUP_PACKET_INIT_SET_INTERFACE(&setup, interface, alternate);
    return controlTransfer(client, device, setup, nullptr);
}

/// Programs HCCHAR.EC (transactions per microframe) and the starting data PID
/// on the host channel ESP-IDF allocated for `endpoint`. ESP-IDF v5.5 never
/// writes EC and starts every channel at DATA0, but a high-bandwidth OUT
/// microframe must start with MDATA and an IN one expects DATA(n-1), as
/// Linux's dwc2 does it. The channel must be idle. False when none matched.
bool programChannel(std::uint8_t deviceAddress, std::uint8_t endpoint, int transactions) {
    bool found = false;
    const std::uint32_t isIn = (endpoint & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) != 0 ? 1 : 0;
    for (int index = 0; index < 16; ++index) {
        volatile usb_dwc_host_chan_regs_t& channel = USB_DWC_HS.host_chans[index];
        usb_dwc_hcchar_reg_t hcchar;
        hcchar.val = channel.hcchar_reg.val;
        if (hcchar.devaddr != deviceAddress || hcchar.epnum != (endpoint & 0x0Fu) ||
            hcchar.epdir != isIn || hcchar.eptype != kDwcIsochronous || hcchar.chena)
            continue;
        std::uint32_t pid = kPidData0;
        if (transactions > 1) pid = isIn ? (transactions == 2 ? kPidData1 : kPidData2) : kPidMData;
        hcchar.ec = static_cast<std::uint32_t>(transactions);
        channel.hcchar_reg.val = hcchar.val;
        usb_dwc_hctsiz_reg_t hctsiz;
        hctsiz.val = channel.hctsiz_reg.val;
        hctsiz.pid = pid;
        channel.hctsiz_reg.val = hctsiz.val;
        found = true;
    }
    return found;
}

bool isTp7Playback(const hal::UsbDeviceInfo& info, const Layout& layout) {
    const Layout::Playback& p = layout.playback;
    return info.vendorId == kTeenageEngineering && p.found &&
           p.channels == static_cast<int>(kTp7MonitorChannels) && p.subslot == 3 &&
           p.intervalMicroframes == 8 &&
           p.maxPacket * p.transactions >= static_cast<int>(98 * kTp7FrameBytes);
}

}  // namespace

Tp7AudioOutput::Tp7AudioOutput(std::shared_ptr<EspUsbStack> stack)
    : stack_{std::move(stack)}, renderExited_{xSemaphoreCreateBinary()} {}

Tp7AudioOutput::~Tp7AudioOutput() {
    close();
    if (renderExited_ != nullptr) vSemaphoreDelete(renderExited_);
}

core::Status Tp7AudioOutput::open(const core::AudioFormat& format, core::FrameCount bufferFrames,
                                  hal::AudioRenderCallback& callback) {
    close();
    if (format != core::kNativeFormat) {
        return core::makeError(core::ErrorCode::UnsupportedFormat,
                               "TP-7 output requires 48 kHz packed-24 stereo");
    }
    if (bufferFrames <= 0) {
        return core::makeError(core::ErrorCode::InvalidArgument, "invalid TP-7 period size");
    }
    if (auto status = stack_->start(); !status) return status;
    lease_ = stack_->acquire(&isTp7Playback);
    if (!lease_) {
        return core::makeError(core::ErrorCode::DeviceNotConnected, "no TP-7 in USB-audio mode");
    }
    playback_ = lease_->layout.playback;
    deviceLost_.store(false);
    if (auto status = configureDevice(); !status) {
        releaseDevice();
        return status;
    }

    const int packets = std::min(kPacketsPerTransfer,
                                 std::max(1, kUsableIsocDescriptors / playback_.intervalMicroframes));
    const std::size_t packetBytes =
        static_cast<std::size_t>(playback_.maxPacket) * static_cast<std::size_t>(playback_.transactions);
    for (int i = 0; i < kTransfers; ++i) {
        usb_transfer_t* transfer = nullptr;
        if (usb_host_transfer_alloc(packetBytes * static_cast<std::size_t>(packets), packets,
                                    &transfer) != ESP_OK)
            break;
        transfer->device_handle = lease_->handle;
        transfer->bEndpointAddress = playback_.endpoint;
        transfer->callback = &Tp7AudioOutput::playbackDone;
        transfer->context = this;
        transfers_.push_back(transfer);
    }
    if (playback_.feedbackEndpoint != 0 && playback_.feedbackMaxPacket > 0 &&
        playback_.feedbackIntervalMicroframes <= kUsableIsocDescriptors) {
        const int polls = std::min(
            kFeedbackPackets, std::max(1, kUsableIsocDescriptors / playback_.feedbackIntervalMicroframes));
        for (int i = 0; i < kFeedbackTransfers; ++i) {
            usb_transfer_t* feedback = nullptr;
            if (usb_host_transfer_alloc(static_cast<std::size_t>(playback_.feedbackMaxPacket * polls),
                                        polls, &feedback) != ESP_OK)
                break;
            feedback->device_handle = lease_->handle;
            feedback->bEndpointAddress = playback_.feedbackEndpoint;
            feedback->callback = &Tp7AudioOutput::feedbackDone;
            feedback->context = this;
            feedback->num_bytes = playback_.feedbackMaxPacket * polls;
            for (int p = 0; p < polls; ++p)
                feedback->isoc_packet_desc[p].num_bytes = playback_.feedbackMaxPacket;
            feedbacks_.push_back(feedback);
        }
    }
    if (static_cast<int>(transfers_.size()) != kTransfers) {
        close();
        return core::makeError(core::ErrorCode::OutOfMemory, "TP-7 playback transfers");
    }

    callback_ = &callback;
    format_ = format;
    periodFrames_ = bufferFrames;
    stereo_.assign(static_cast<std::size_t>(bufferFrames) * format.channels, 0.0F);
    wide_.assign(static_cast<std::size_t>(bufferFrames) * 2U * kTp7MonitorChannels, 0.0F);
    packed_.assign(static_cast<std::size_t>(bufferFrames) * 2U * kTp7FrameBytes, 0);
    fifo_ = std::make_unique<FrameFifo>(kTp7FrameBytes, kFifoFrames);
    core::log::info(kModule, "TP-7 playback ready: interface " + std::to_string(playback_.interface) +
                                 " alt " + std::to_string(playback_.alternate) + ", " +
                                 std::to_string(playback_.maxPacket) + " B x " +
                                 std::to_string(playback_.transactions) + " per microframe" +
                                 (feedbacks_.empty() ? ", NO feedback endpoint" : ", feedback endpoint"));
    return core::success();
}

core::Status Tp7AudioOutput::configureDevice() {
    const usb_host_client_handle_t client = stack_->client();
    const usb_device_handle_t device = lease_->handle;
    if (!claimed_) {
        if (const esp_err_t err =
                usb_host_interface_claim(client, device, playback_.interface, playback_.alternate);
            err != ESP_OK) {
            return core::makeError(core::ErrorCode::DeviceBusy,
                                   std::string{"cannot claim TP-7 playback: "} + esp_err_to_name(err));
        }
        claimed_ = true;
    }
    if (setInterface(client, device, playback_.interface, playback_.alternate) != ESP_OK) {
        return core::makeError(core::ErrorCode::IoError, "TP-7 SET_INTERFACE failed");
    }
    if (!programChannel(lease_->info.deviceAddress, playback_.endpoint, playback_.transactions)) {
        return core::makeError(core::ErrorCode::IoError, "no idle host channel for TP-7 playback");
    }
    if (playback_.feedbackEndpoint != 0)
        (void)programChannel(lease_->info.deviceAddress, playback_.feedbackEndpoint, 1);

    // UAC1 SET_CUR of the endpoint's sampling frequency, as Linux sends it.
    usb_setup_packet_t setup{};
    setup.bmRequestType = 0x22;  // Host to device, class, endpoint.
    setup.bRequest = 0x01;       // SET_CUR
    setup.wValue = 0x0100;       // SAMPLING_FREQ_CONTROL
    setup.wIndex = playback_.endpoint;
    setup.wLength = 3;
    std::uint8_t rate[3] = {kTp7MonitorRate & 0xFF, (kTp7MonitorRate >> 8) & 0xFF,
                            (kTp7MonitorRate >> 16) & 0xFF};
    (void)controlTransfer(client, device, setup, rate);  // A fixed-rate endpoint may refuse it.

    // The master mute of feature unit 3 (audio control interface 0). Another
    // host may have left it set.
    setup = {};
    setup.bmRequestType = 0x21;  // Host to device, class, interface.
    setup.bRequest = 0x01;       // SET_CUR
    setup.wValue = 0x0100;       // MUTE_CONTROL, master channel
    setup.wIndex = (3 << 8) | 0;
    setup.wLength = 1;
    std::uint8_t off = 0;
    (void)controlTransfer(client, device, setup, &off);
    return core::success();
}

core::Status Tp7AudioOutput::start() {
    if (!lease_ || callback_ == nullptr) {
        return core::makeError(core::ErrorCode::InvalidState, "TP-7 output not open");
    }
    if (running_.load()) return core::success();

    // Silence for the transfers queued at the start, plus the FIFO target, so
    // the first completions have something to send before the render task
    // has run.
    fifo_->reset();
    std::vector<std::uint8_t> silence(
        (kTargetFrames + kTransfers * kPacketsPerTransfer * 98U) * kTp7FrameBytes, 0);
    (void)fifo_->write(silence);
    converter_ = {};
    reportedUnderruns_ = underruns_.load();

    rendering_.store(true);
    if (xTaskCreatePinnedToCore(&Tp7AudioOutput::renderTask, "tp7_render", 16384, this, 17,
                                &renderTask_, 1) != pdPASS) {
        rendering_.store(false);
        renderTask_ = nullptr;
        return core::makeError(core::ErrorCode::OutOfMemory, "TP-7 render task");
    }
    startStream();
    if (inFlight_.load() == 0) {
        stop();
        return core::makeError(core::ErrorCode::IoError, "TP-7 playback submit failed");
    }
    running_.store(true);
    return core::success();
}

void Tp7AudioOutput::startStream() {
    sizer_.reset();
    sizer_.setRateQ16(static_cast<std::uint32_t>((std::uint64_t{kTp7MonitorRate} << 16) / 1000));
    streaming_.store(true, std::memory_order_release);
    for (usb_transfer_t* feedback : feedbacks_) {
        inFlight_.fetch_add(1);
        if (usb_host_transfer_submit(feedback) != ESP_OK) inFlight_.fetch_sub(1);
    }
    for (usb_transfer_t* transfer : transfers_) {
        fillPacket(transfer);
        inFlight_.fetch_add(1);
        if (usb_host_transfer_submit(transfer) != ESP_OK) inFlight_.fetch_sub(1);
    }
    const std::int64_t now = esp_timer_get_time();
    streamStartedUs_ = now;
    feedbackCheckedUs_ = now;
    feedbackAtCheck_ = feedbackReports_.load();
    resetRequested_ = false;
    loggedNoFeedback_ = false;
}

void Tp7AudioOutput::stopStream() noexcept {
    streaming_.store(false, std::memory_order_release);
    // Isochronous transfers retire on their own within a few milliseconds.
    const std::int64_t deadline = esp_timer_get_time() + 500'000;
    while (inFlight_.load() > 0 && esp_timer_get_time() < deadline) vTaskDelay(pdMS_TO_TICKS(2));
    if (inFlight_.load() > 0 && lease_) {
        for (const std::uint8_t endpoint : {playback_.endpoint, playback_.feedbackEndpoint}) {
            if (endpoint == 0) continue;
            (void)usb_host_endpoint_halt(lease_->handle, endpoint);
            (void)usb_host_endpoint_flush(lease_->handle, endpoint);
            (void)usb_host_endpoint_clear(lease_->handle, endpoint);
        }
        while (inFlight_.load() > 0) vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void Tp7AudioOutput::stop() noexcept {
    running_.store(false);
    if (renderTask_ != nullptr) {
        rendering_.store(false);
        xTaskNotifyGive(renderTask_);
        xSemaphoreTake(renderExited_, portMAX_DELAY);
        renderTask_ = nullptr;
    }
    stopStream();
}

void Tp7AudioOutput::close() noexcept {
    stop();
    for (usb_transfer_t* transfer : transfers_) usb_host_transfer_free(transfer);
    transfers_.clear();
    for (usb_transfer_t* feedback : feedbacks_) usb_host_transfer_free(feedback);
    feedbacks_.clear();
    releaseDevice();
    callback_ = nullptr;
    fifo_.reset();
    stereo_.clear();
    wide_.clear();
    packed_.clear();
}

void Tp7AudioOutput::releaseDevice() noexcept {
    if (!lease_) return;
    if (claimed_) {
        // Alternate 0 has no endpoints: the TP-7 stops expecting audio.
        if (!deviceLost_.load() && !stack_->isGone(*lease_))
            (void)setInterface(stack_->client(), lease_->handle, playback_.interface, 0);
        (void)usb_host_interface_release(stack_->client(), lease_->handle, playback_.interface);
        claimed_ = false;
    }
    stack_->release(*lease_);
    lease_.reset();
}

bool Tp7AudioOutput::isRunning() const noexcept {
    return running_.load() && !deviceLost_.load() && lease_ && !stack_->isGone(*lease_);
}

core::FrameCount Tp7AudioOutput::latencyFrames() const noexcept {
    // FIFO target plus the transfers queued ahead of the bus, at 96 kHz.
    const std::size_t queued = kTargetFrames + kTransfers * kPacketsPerTransfer * 96U;
    return static_cast<core::FrameCount>(queued / 2U);
}

core::Status Tp7AudioOutput::refresh() {
    if (!isRunning() || feedbacks_.empty() || resetRequested_) return core::success();
    const std::int64_t now = esp_timer_get_time();
    const std::uint32_t reports = feedbackReports_.load();
    if (reports != feedbackAtCheck_) {
        feedbackAtCheck_ = reports;
        feedbackCheckedUs_ = now;
        consecutiveResets_ = 0;
        return core::success();
    }
    if (now - feedbackCheckedUs_ < kFeedbackTimeoutUs) return core::success();
    if (!resetHook_ || consecutiveResets_ >= kMaxResets) {
        if (!loggedNoFeedback_) {
            core::log::warning(kModule, "the TP-7 sends no playback rate; playing at the nominal "
                                        "96 kHz, which drifts");
            loggedNoFeedback_ = true;
        }
        return core::success();
    }
    core::log::warning(kModule, "no playback rate from the TP-7 for " +
                                    std::to_string((now - feedbackCheckedUs_) / 1000) + " ms (" +
                                    std::to_string(emptyFeedback_.load()) +
                                    " empty reports); sending it through MTP mode to restart it");
    ++consecutiveResets_;
    resets_.fetch_add(1);
    resetRequested_ = true;
    resetHook_();
    return core::success();
}

Tp7AudioOutput::Stats Tp7AudioOutput::stats() const noexcept {
    Stats stats;
    stats.packets = packets_.load();
    stats.frames = frames_.load();
    stats.underruns = underruns_.load();
    stats.failedPackets = failedPackets_.load();
    stats.feedbackReports = feedbackReports_.load();
    stats.emptyFeedback = emptyFeedback_.load();
    stats.framesPerMsQ16 = sizer_.rateQ16();
    stats.resets = resets_.load();
    stats.fifoFrames = fifo_ ? static_cast<std::uint32_t>(fifo_->availableFrames()) : 0;
    return stats;
}

void Tp7AudioOutput::renderTask(void* context) {
    auto& self = *static_cast<Tp7AudioOutput*>(context);
    auto* packed = reinterpret_cast<std::byte*>(self.packed_.data());
    while (self.rendering_.load(std::memory_order_acquire)) {
        if (!self.streaming_.load(std::memory_order_acquire) ||
            self.fifo_->availableFrames() >= kTargetFrames) {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
            continue;
        }
        self.callback_->renderAudio(self.stereo_, self.periodFrames_, self.format_.channels);
        convertStereo48ToTp7Monitor96(self.stereo_, self.wide_, self.converter_);
        for (std::size_t sample = 0; sample < self.wide_.size(); ++sample)
            packS24_3LE(self.wide_[sample], std::span<std::byte, 3>{packed + sample * 3U, 3U});
        (void)self.fifo_->write(self.packed_);
        const std::uint32_t underruns = self.underruns_.load(std::memory_order_relaxed);
        if (underruns != self.reportedUnderruns_) {
            self.reportedUnderruns_ = underruns;
            self.callback_->onXRun();
        }
    }
    xSemaphoreGive(self.renderExited_);
    vTaskDelete(nullptr);
}

void Tp7AudioOutput::fillPacket(usb_transfer_t* transfer) noexcept {
    const auto maxFrames = static_cast<std::uint32_t>(
        playback_.maxPacket * playback_.transactions / static_cast<int>(kTp7FrameBytes));
    std::uint8_t* out = transfer->data_buffer;
    int total = 0;
    for (int packet = 0; packet < transfer->num_isoc_packets; ++packet) {
        const std::uint32_t frames = sizer_.next(maxFrames);
        const std::size_t got = fifo_->read(out + total, frames);
        if (got < frames) {
            std::memset(out + total + got * kTp7FrameBytes, 0, (frames - got) * kTp7FrameBytes);
            underruns_.fetch_add(1, std::memory_order_relaxed);
        }
        const int bytes = static_cast<int>(frames * kTp7FrameBytes);
        transfer->isoc_packet_desc[packet].num_bytes = bytes;
        total += bytes;
        frames_.fetch_add(frames, std::memory_order_relaxed);
    }
    transfer->num_bytes = total;
}

void Tp7AudioOutput::playbackDone(usb_transfer_t* transfer) {
    auto& self = *static_cast<Tp7AudioOutput*>(transfer->context);
    for (int packet = 0; packet < transfer->num_isoc_packets; ++packet) {
        self.packets_.fetch_add(1, std::memory_order_relaxed);
        if (transfer->isoc_packet_desc[packet].status != USB_TRANSFER_STATUS_COMPLETED)
            self.failedPackets_.fetch_add(1, std::memory_order_relaxed);
    }
    if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE) self.deviceLost_.store(true);
    if (self.streaming_.load(std::memory_order_acquire) && !self.deviceLost_.load()) {
        self.fillPacket(transfer);
        if (usb_host_transfer_submit(transfer) == ESP_OK) {
            if (self.renderTask_ != nullptr) xTaskNotifyGive(self.renderTask_);
            return;
        }
    }
    self.inFlight_.fetch_sub(1);
}

void Tp7AudioOutput::feedbackDone(usb_transfer_t* transfer) {
    auto& self = *static_cast<Tp7AudioOutput*>(transfer->context);
    const int stride = self.playback_.feedbackMaxPacket;
    for (int packet = 0; packet < transfer->num_isoc_packets; ++packet) {
        const usb_isoc_packet_desc_t& desc = transfer->isoc_packet_desc[packet];
        if (desc.status != USB_TRANSFER_STATUS_COMPLETED) continue;
        if (desc.actual_num_bytes < 3) {
            self.emptyFeedback_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const std::uint8_t* bytes = transfer->data_buffer + packet * stride;
        std::uint32_t raw = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16);
        if (desc.actual_num_bytes >= 4) raw |= static_cast<std::uint32_t>(bytes[3]) << 24;
        if (const auto rate = decodeUac1Feedback(raw, kTp7MonitorRate)) {
            self.sizer_.setRateQ16(*rate);
            self.feedbackReports_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE) self.deviceLost_.store(true);
    if (self.streaming_.load(std::memory_order_acquire) && !self.deviceLost_.load() &&
        usb_host_transfer_submit(transfer) == ESP_OK)
        return;
    self.inFlight_.fetch_sub(1);
}

}  // namespace aw::platform::esp32p4::usb
