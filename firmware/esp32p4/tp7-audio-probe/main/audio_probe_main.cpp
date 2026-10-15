///
/// TP-7 monitor-audio probe (port step P3a).
///
/// Streams a test tone to a TP-7 in USB-audio mode over its UAC1 playback
/// endpoint, and reports what the bus did. It answers the questions that
/// decide whether Bridge can play through the TP-7 from an ESP32-P4:
///
///   1. Does the host channel accept a high-bandwidth isochronous OUT endpoint
///      (2 x 882 bytes per microframe) once its multi-count is programmed?
///   2. Does every packet complete, for minutes, with nothing skipped?
///   3. What does the asynchronous feedback endpoint report, and does the
///      stream track it (about 96 000 frames per second)?
///   4. Is the tone clean on the TP-7's output?
///
/// The probe never switches the TP-7 into MTP mode and never touches storage.
///
#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "UsbClient.hpp"
#include "driver/gpio.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/usb_dwc_struct.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"

namespace {

constexpr const char* kTag = "tp7-audio";
constexpr std::uint16_t kTeenageEngineering = 0x2367;
constexpr std::uint16_t kTp7MtpProduct = 0x0019;
constexpr std::uint8_t kClassAudio = 0x01;
constexpr std::uint8_t kSubclassAudioStreaming = 0x02;
constexpr std::uint8_t kDescriptorCsInterface = 0x24;
constexpr std::uint8_t kAsFormatType = 0x02;

constexpr std::uint32_t kSampleRate = 96000;
/// ESP-IDF keeps 64 isochronous descriptors per transfer, 3 of them margin.
constexpr int kUsableIsocDescriptors = 64 - 3;

#ifdef CONFIG_PROBE_HIGH_BANDWIDTH
constexpr bool kHighBandwidth = true;
#else
constexpr bool kHighBandwidth = false;
#endif

/// The DWC2 host channel types, as HCCHAR.EPType encodes them.
constexpr std::uint32_t kDwcIsochronous = 1;

// ---------------------------------------------------------------- descriptors

struct Playback {
    bool found{false};
    std::uint8_t interface{0};
    std::uint8_t alternate{0};
    std::uint8_t endpoint{0};
    std::uint16_t maxPacket{0};        ///< Bytes per transaction.
    int transactions{1};               ///< Per microframe (bits 12:11 + 1).
    int intervalMicroframes{0};
    int channels{0};
    int subslot{0};
    int bits{0};
    std::uint8_t feedbackEndpoint{0};  ///< bSynchAddress; 0 = none.
    std::uint16_t feedbackMaxPacket{0};
    int feedbackIntervalMicroframes{0};

    [[nodiscard]] int frameBytes() const { return channels * subslot; }
    [[nodiscard]] int maxFramesPerPacket() const {
        return frameBytes() > 0 ? maxPacket * transactions / frameBytes() : 0;
    }
};

/// Finds the first audio-streaming alternate setting with an isochronous OUT
/// data endpoint, and the feedback endpoint its bSynchAddress names.
Playback findPlayback(const usb_config_desc_t* config) {
    Playback playback;
    Playback candidate;
    int offset = 0;
    const auto* descriptor = reinterpret_cast<const usb_standard_desc_t*>(config);
    const usb_intf_desc_t* current = nullptr;
    struct IsocIn {
        std::uint8_t address;
        std::uint16_t maxPacket;
        int interval;
    };
    std::vector<IsocIn> inEndpoints;

    auto finishInterface = [&] {
        if (playback.found || !candidate.found) return;
        for (const IsocIn& in : inEndpoints) {
            if (in.address == candidate.feedbackEndpoint) {
                candidate.feedbackMaxPacket = in.maxPacket;
                candidate.feedbackIntervalMicroframes = in.interval;
            }
        }
        playback = candidate;
    };

    while ((descriptor = usb_parse_next_descriptor(descriptor, config->wTotalLength, &offset))) {
        const auto* raw = reinterpret_cast<const std::uint8_t*>(descriptor);
        if (descriptor->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            finishInterface();
            current = reinterpret_cast<const usb_intf_desc_t*>(descriptor);
            candidate = {};
            inEndpoints.clear();
            candidate.interface = current->bInterfaceNumber;
            candidate.alternate = current->bAlternateSetting;
            continue;
        }
        if (current == nullptr || current->bInterfaceClass != kClassAudio ||
            current->bInterfaceSubClass != kSubclassAudioStreaming)
            continue;
        if (descriptor->bDescriptorType == kDescriptorCsInterface && raw[0] >= 8 &&
            raw[2] == kAsFormatType) {
            candidate.channels = raw[4];
            candidate.subslot = raw[5];
            candidate.bits = raw[6];
        } else if (descriptor->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const auto* ep = reinterpret_cast<const usb_ep_desc_t*>(descriptor);
            if ((ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) != USB_BM_ATTRIBUTES_XFER_ISOC)
                continue;
            const bool isIn = (ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) != 0;
            const int interval = 1 << (ep->bInterval - 1);  // High speed.
            if (isIn) {
                inEndpoints.push_back({ep->bEndpointAddress,
                                       static_cast<std::uint16_t>(ep->wMaxPacketSize & 0x7FF),
                                       interval});
            } else if (!candidate.found) {
                candidate.found = true;
                candidate.endpoint = ep->bEndpointAddress;
                candidate.maxPacket = ep->wMaxPacketSize & 0x7FF;
                candidate.transactions = 1 + ((ep->wMaxPacketSize >> 11) & 3);
                candidate.intervalMicroframes = interval;
                // UAC1 endpoints are 9 bytes: bRefresh, then bSynchAddress.
                candidate.feedbackEndpoint = raw[0] >= 9 ? raw[8] : 0;
            }
        }
    }
    finishInterface();
    return playback;
}

// ------------------------------------------------------------ control helpers

/// Runs one control transfer on EP0 and waits for it. `data` is sent for an
/// OUT request and filled for an IN request.
esp_err_t controlTransfer(usb_host_client_handle_t client, usb_device_handle_t device,
                          const usb_setup_packet_t& setup, std::uint8_t* data,
                          usb_transfer_status_t& status) {
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (done == nullptr) return ESP_ERR_NO_MEM;
    usb_transfer_t* transfer = nullptr;
    esp_err_t err = usb_host_transfer_alloc(USB_SETUP_PACKET_SIZE + 64, 0, &transfer);
    if (err == ESP_OK) {
        std::memcpy(transfer->data_buffer, &setup, USB_SETUP_PACKET_SIZE);
        const bool isIn = (setup.bmRequestType & USB_BM_REQUEST_TYPE_DIR_IN) != 0;
        if (!isIn) std::memcpy(transfer->data_buffer + USB_SETUP_PACKET_SIZE, data, setup.wLength);
        transfer->num_bytes = USB_SETUP_PACKET_SIZE + setup.wLength;
        transfer->device_handle = device;
        transfer->bEndpointAddress = 0;
        transfer->callback = [](usb_transfer_t* t) {
            xSemaphoreGive(static_cast<SemaphoreHandle_t>(t->context));
        };
        transfer->context = done;
        err = usb_host_transfer_submit_control(client, transfer);
        if (err == ESP_OK) {
            xSemaphoreTake(done, portMAX_DELAY);
            status = transfer->status;
            if (status != USB_TRANSFER_STATUS_COMPLETED) {
                err = ESP_FAIL;
            } else if (isIn) {
                std::memcpy(data, transfer->data_buffer + USB_SETUP_PACKET_SIZE, setup.wLength);
            }
        }
        usb_host_transfer_free(transfer);
    }
    vSemaphoreDelete(done);
    return err;
}

/// Selects an alternate setting on the device. `usb_host_interface_claim`
/// only allocates the host's pipes; it never sends SET_INTERFACE, so without
/// this the TP-7 stays on alternate 0, which has no endpoints: it ignores the
/// playback data and never answers on the feedback endpoint.
esp_err_t setInterface(usb_host_client_handle_t client, usb_device_handle_t device,
                       std::uint8_t interface, std::uint8_t alternate) {
    usb_setup_packet_t setup{};
    USB_SETUP_PACKET_INIT_SET_INTERFACE(&setup, interface, alternate);
    usb_transfer_status_t status = USB_TRANSFER_STATUS_COMPLETED;
    std::uint8_t none = 0;
    const esp_err_t err = controlTransfer(client, device, setup, &none, status);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "SET_INTERFACE %u alt %u: %s", interface, alternate,
                 probe::transferStatusName(status));
        return err;
    }
    setup.bmRequestType = 0x81;  // Device to host, standard, interface.
    setup.bRequest = 0x0A;       // GET_INTERFACE
    setup.wValue = 0;
    setup.wIndex = interface;
    setup.wLength = 1;
    std::uint8_t current = 0xFF;
    if (controlTransfer(client, device, setup, &current, status) == ESP_OK)
        ESP_LOGI(kTag, "interface %u now on alternate %u", interface, current);
    return ESP_OK;
}

/// UAC1 SET_CUR / GET_CUR of the endpoint's sampling-frequency control.
void setSampleRate(usb_host_client_handle_t client, usb_device_handle_t device,
                   std::uint8_t endpoint) {
    usb_setup_packet_t setup{};
    setup.bmRequestType = 0x22;  // Host to device, class, endpoint.
    setup.bRequest = 0x01;       // SET_CUR
    setup.wValue = 0x0100;       // SAMPLING_FREQ_CONTROL
    setup.wIndex = endpoint;
    setup.wLength = 3;
    std::uint8_t rate[3] = {kSampleRate & 0xFF, (kSampleRate >> 8) & 0xFF, kSampleRate >> 16};
    usb_transfer_status_t status = USB_TRANSFER_STATUS_COMPLETED;
    if (controlTransfer(client, device, setup, rate, status) != ESP_OK) {
        // A device with one fixed rate may not implement the control at all.
        ESP_LOGW(kTag, "SET_CUR sampling frequency: %s (fine for a fixed-rate endpoint)",
                 probe::transferStatusName(status));
        return;
    }
    setup.bmRequestType = 0xA2;  // Device to host, class, endpoint.
    setup.bRequest = 0x81;       // GET_CUR
    std::memset(rate, 0, sizeof rate);
    if (controlTransfer(client, device, setup, rate, status) == ESP_OK) {
        ESP_LOGI(kTag, "sampling frequency set; device reports %u Hz",
                 rate[0] | (rate[1] << 8) | (rate[2] << 16));
    } else {
        ESP_LOGI(kTag, "sampling frequency set (GET_CUR: %s)", probe::transferStatusName(status));
    }
}

// ------------------------------------------------------- host channel registers

/// One way of programming the playback channel. The probe cycles through
/// these, each with its own tone, so one listening pass shows which works.
struct Variant {
    const char* name;
    int toneHz;
    std::uint32_t pid;  ///< HCTSIZ.PID: 0 DATA0, 1 DATA2, 2 DATA1, 3 MDATA.
    /// Which half of each 1 ms packet carries the tone (bit 0 frames 0-47,
    /// bit 1 the rest); the other half is silence.
    unsigned transactionsWithTone;
};
constexpr Variant kVariants[] = {
    {"tone in frames 0-47 of each packet only", 440, 3, 1},
    {"tone in frames 48-95 of each packet only", 880, 3, 2},
    {"reference: 2000 Hz in every frame", 2000, 3, 3},
};
constexpr int kVariantSeconds = 8;
int g_variant = 0;
std::uint8_t g_address = 0;

/// HCTSIZ.PID encodings.
constexpr std::uint32_t kPidData0 = 0;
constexpr std::uint32_t kPidData2 = 1;
constexpr std::uint32_t kPidData1 = 2;

/// Programs HCCHAR.EC/MC ("transactions per microframe") on the host channel
/// that ESP-IDF allocated for this endpoint, and the starting data PID that
/// goes with it. ESP-IDF v5.5.1 never writes EC, and sets the PID to DATA0
/// once when it allocates the channel. A high-bandwidth OUT microframe must
/// start with MDATA (then DATA1), and an IN one expects DATA(n-1); with DATA0
/// the device drops the data. Linux's dwc2 picks the PID the same way
/// (dwc2_set_pid_isoc). The channel must be idle: call after claiming the
/// interface, before the first transfer. Returns the channel index, or -1.
int setTransactionsPerMicroframe(std::uint8_t deviceAddress, std::uint8_t endpoint,
                                 int transactions) {
    int found = -1;
    for (int index = 0; index < 16; ++index) {
        volatile usb_dwc_host_chan_regs_t& channel = USB_DWC_HS.host_chans[index];
        usb_dwc_hcchar_reg_t hcchar;
        hcchar.val = channel.hcchar_reg.val;
        const std::uint32_t isIn = (endpoint & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) ? 1 : 0;
        if (hcchar.devaddr != deviceAddress || hcchar.epnum != (endpoint & 0x0F) ||
            hcchar.epdir != isIn || hcchar.eptype != kDwcIsochronous || hcchar.chena)
            continue;
        std::uint32_t pid = kPidData0;
        if (transactions > 1)
            pid = isIn ? (transactions == 2 ? kPidData1 : kPidData2) : kVariants[g_variant].pid;
        usb_dwc_hctsiz_reg_t hctsiz;
        hctsiz.val = channel.hctsiz_reg.val;
        ESP_LOGI(kTag, "host channel %d (ep 0x%02x): EC %u -> %d, PID %u -> %u", index, endpoint,
                 static_cast<unsigned>(hcchar.ec), transactions, static_cast<unsigned>(hctsiz.pid),
                 static_cast<unsigned>(pid));
        hcchar.ec = static_cast<std::uint32_t>(transactions);
        channel.hcchar_reg.val = hcchar.val;
        hctsiz.pid = pid;
        channel.hctsiz_reg.val = hctsiz.val;
        found = index;
    }
    return found;
}

// ------------------------------------------------------------------ streaming

struct Stats {
    std::atomic<std::uint32_t> urbs{0};
    std::atomic<std::uint32_t> packets{0};
    std::atomic<std::uint32_t> badPackets{0};
    std::atomic<std::uint32_t> badUrbs{0};
    std::atomic<std::uint32_t> lastBadStatus{USB_TRANSFER_STATUS_COMPLETED};
    std::atomic<std::uint64_t> frames{0};
    std::atomic<std::uint32_t> feedbackRaw{0};
    std::atomic<std::uint32_t> feedbackPackets{0};
    std::atomic<std::uint32_t> feedbackErrors{0};
    std::atomic<std::uint32_t> lastFeedbackStatus{USB_TRANSFER_STATUS_COMPLETED};
    std::atomic<std::uint32_t> lastFeedbackBytes{0};
    /// Microframes between playback completions since the last report.
    std::atomic<std::uint32_t> minGap{UINT32_MAX};
    std::atomic<std::uint32_t> maxGap{0};
    std::atomic<std::uint32_t> minFrames{UINT32_MAX};
    std::atomic<std::uint32_t> maxFrames{0};

    void reset() {
        for (auto* counter : {&urbs, &packets, &badPackets, &badUrbs, &feedbackRaw,
                              &feedbackPackets, &feedbackErrors})
            counter->store(0);
        lastBadStatus.store(USB_TRANSFER_STATUS_COMPLETED);
        lastFeedbackStatus.store(USB_TRANSFER_STATUS_COMPLETED);
        lastFeedbackBytes.store(0);
        frames.store(0);
    }
};

/// Everything the transfer callbacks touch. They all run on the USB client
/// task, one at a time, so only what the main task reads is atomic.
struct Stream {
    Playback playback;
    usb_device_handle_t device{nullptr};
    std::atomic<bool> running{false};
    std::atomic<int> inFlight{0};
    std::vector<usb_transfer_t*> transfers;
    usb_transfer_t* feedback{nullptr};
    Stats stats;

    /// Frames per millisecond in Q16.16. Starts at nominal; the feedback
    /// endpoint corrects it.
    std::atomic<std::uint32_t> rateQ16{(kSampleRate / 1000) << 16};
    std::atomic<const char*> feedbackFormat{"none yet"};
    std::uint32_t remainderQ16{0};
    std::int32_t lastCompletion{-1};  ///< HFNUM microframe of the last completion.
    double phase{0.0};
    double phaseStep{0.0};
    float amplitude{0.0F};
};

Stream g_stream;

/// Interprets a feedback value the way Linux's snd-usb-audio does: try each
/// encoding a device may use and keep the one that lands near nominal.
void applyFeedback(Stream& stream, std::uint32_t raw) {
    const double nominal = kSampleRate / 1000.0;
    struct Candidate {
        const char* name;
        double framesPerMs;
    };
    const Candidate candidates[] = {
        {"Q16.16 per microframe", raw / 65536.0 * 8.0},
        {"Q16.16 per frame", raw / 65536.0},
        {"Q10.14 per frame", raw / 16384.0},
    };
    for (const Candidate& candidate : candidates) {
        if (std::fabs(candidate.framesPerMs - nominal) < nominal * 0.05) {
            stream.rateQ16.store(static_cast<std::uint32_t>(candidate.framesPerMs * 65536.0),
                                 std::memory_order_relaxed);
            stream.feedbackFormat.store(candidate.name, std::memory_order_relaxed);
            return;
        }
    }
}

/// Fills one playback transfer with the tone, frame counts following the
/// feedback rate, and returns the bytes it holds.
int fillPlayback(Stream& stream, usb_transfer_t* transfer) {
    const Playback& p = stream.playback;
    const int frameBytes = p.frameBytes();
    const int maxFrames = p.maxFramesPerPacket();
    std::uint8_t* out = transfer->data_buffer;
    int total = 0;
    for (int packet = 0; packet < transfer->num_isoc_packets; ++packet) {
        const std::uint32_t due =
            stream.remainderQ16 + stream.rateQ16.load(std::memory_order_relaxed);
        int frames = static_cast<int>(due >> 16);
        stream.remainderQ16 = due & 0xFFFF;
        frames = std::clamp(frames, 1, maxFrames);
        const int firstHalfFrames = kSampleRate / 2000;
        for (int frame = 0; frame < frames; ++frame) {
            const unsigned transactionBit = frame < firstHalfFrames ? 1U : 2U;
            const float gate =
                (kVariants[g_variant].transactionsWithTone & transactionBit) != 0 ? 1.0F : 0.0F;
            const float value =
                gate * stream.amplitude * static_cast<float>(std::sin(stream.phase));
            stream.phase += stream.phaseStep;
            if (stream.phase > 2.0 * M_PI) stream.phase -= 2.0 * M_PI;
            const auto sample = static_cast<std::int32_t>(std::lrint(value * 8388607.0F));
            std::uint8_t* slot = out + total + frame * frameBytes;
            std::memset(slot, 0, static_cast<std::size_t>(frameBytes));
            for (int channel = 0; channel < 2 && channel < p.channels; ++channel) {
                std::uint8_t* bytes = slot + channel * p.subslot + (p.subslot - 3);
                bytes[0] = static_cast<std::uint8_t>(sample & 0xFF);
                bytes[1] = static_cast<std::uint8_t>((sample >> 8) & 0xFF);
                bytes[2] = static_cast<std::uint8_t>((sample >> 16) & 0xFF);
            }
        }
        transfer->isoc_packet_desc[packet].num_bytes = frames * frameBytes;
        const auto f = static_cast<std::uint32_t>(frames);
        if (f < stream.stats.minFrames.load(std::memory_order_relaxed))
            stream.stats.minFrames.store(f, std::memory_order_relaxed);
        if (f > stream.stats.maxFrames.load(std::memory_order_relaxed))
            stream.stats.maxFrames.store(f, std::memory_order_relaxed);
        total += frames * frameBytes;
        stream.stats.frames.fetch_add(static_cast<std::uint64_t>(frames), std::memory_order_relaxed);
    }
    transfer->num_bytes = total;
    return total;
}

void playbackDone(usb_transfer_t* transfer) {
    Stream& stream = *static_cast<Stream*>(transfer->context);
    Stats& stats = stream.stats;
    stats.urbs.fetch_add(1, std::memory_order_relaxed);
    // HFNUM counts microframes at high speed, in 14 bits.
    const auto now = static_cast<std::int32_t>(USB_DWC_HS.hfnum_reg.frnum & 0x3FFF);
    if (stream.lastCompletion >= 0) {
        const auto gap = static_cast<std::uint32_t>((now - stream.lastCompletion) & 0x3FFF);
        if (gap < stats.minGap.load(std::memory_order_relaxed))
            stats.minGap.store(gap, std::memory_order_relaxed);
        if (gap > stats.maxGap.load(std::memory_order_relaxed))
            stats.maxGap.store(gap, std::memory_order_relaxed);
    }
    stream.lastCompletion = now;
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        stats.badUrbs.fetch_add(1, std::memory_order_relaxed);
        stats.lastBadStatus.store(transfer->status, std::memory_order_relaxed);
    }
    for (int packet = 0; packet < transfer->num_isoc_packets; ++packet) {
        const usb_isoc_packet_desc_t& desc = transfer->isoc_packet_desc[packet];
        stats.packets.fetch_add(1, std::memory_order_relaxed);
        if (desc.status != USB_TRANSFER_STATUS_COMPLETED) {
            stats.badPackets.fetch_add(1, std::memory_order_relaxed);
            stats.lastBadStatus.store(desc.status, std::memory_order_relaxed);
        }
    }
    if (stream.running.load(std::memory_order_acquire) &&
        transfer->status != USB_TRANSFER_STATUS_NO_DEVICE) {
        fillPlayback(stream, transfer);
        if (usb_host_transfer_submit(transfer) == ESP_OK) return;
    }
    stream.inFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void feedbackDone(usb_transfer_t* transfer) {
    Stream& stream = *static_cast<Stream*>(transfer->context);
    const int stride = stream.playback.feedbackMaxPacket;
    for (int packet = 0; packet < transfer->num_isoc_packets; ++packet) {
        const usb_isoc_packet_desc_t& desc = transfer->isoc_packet_desc[packet];
        stream.stats.lastFeedbackBytes.store(static_cast<std::uint32_t>(desc.actual_num_bytes),
                                             std::memory_order_relaxed);
        if (desc.status != USB_TRANSFER_STATUS_COMPLETED || desc.actual_num_bytes < 3) {
            if (desc.status != USB_TRANSFER_STATUS_COMPLETED) {
                stream.stats.feedbackErrors.fetch_add(1, std::memory_order_relaxed);
                stream.stats.lastFeedbackStatus.store(desc.status, std::memory_order_relaxed);
            }
            continue;
        }
        const std::uint8_t* bytes = transfer->data_buffer + packet * stride;
        std::uint32_t raw = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16);
        if (desc.actual_num_bytes >= 4) raw |= static_cast<std::uint32_t>(bytes[3]) << 24;
        stream.stats.feedbackRaw.store(raw, std::memory_order_relaxed);
        stream.stats.feedbackPackets.fetch_add(1, std::memory_order_relaxed);
        applyFeedback(stream, raw);
    }
    if (stream.running.load(std::memory_order_acquire) &&
        transfer->status != USB_TRANSFER_STATUS_NO_DEVICE &&
        usb_host_transfer_submit(transfer) == ESP_OK)
        return;
    stream.inFlight.fetch_sub(1, std::memory_order_acq_rel);
}

bool startStream(usb_host_client_handle_t client, usb_device_handle_t device,
                 std::uint8_t address, const Playback& playback) {
    Stream& stream = g_stream;
    stream.playback = playback;
    stream.device = device;
    stream.stats.reset();
    stream.rateQ16.store((kSampleRate / 1000) << 16);
    stream.feedbackFormat.store("none yet");
    stream.remainderQ16 = 0;
    stream.lastCompletion = -1;
    stream.phase = 0.0;
    stream.phaseStep = 2.0 * M_PI * kVariants[g_variant].toneHz / kSampleRate;
    ESP_LOGW(kTag, "==== variant %d: %s -> %d Hz tone", g_variant + 1, kVariants[g_variant].name,
             kVariants[g_variant].toneHz);
    stream.amplitude = static_cast<float>(std::pow(10.0, CONFIG_PROBE_TONE_DBFS / 20.0));

    if (const esp_err_t err =
            usb_host_interface_claim(client, device, playback.interface, playback.alternate);
        err != ESP_OK) {
        ESP_LOGE(kTag, "claim interface %u alt %u failed: %s", playback.interface,
                 playback.alternate, esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(kTag, "claimed interface %u alt %u", playback.interface, playback.alternate);
    if (setInterface(client, device, playback.interface, playback.alternate) != ESP_OK) {
        usb_host_interface_release(client, device, playback.interface);
        return false;
    }

    if (kHighBandwidth && playback.transactions > 1) {
        if (setTransactionsPerMicroframe(address, playback.endpoint, playback.transactions) < 0)
            ESP_LOGE(kTag, "no idle host channel matches ep 0x%02x; multi-count not set",
                     playback.endpoint);
    } else if (playback.transactions > 1) {
        ESP_LOGW(kTag, "high-bandwidth programming disabled in menuconfig: expect failures");
    }
    if (playback.feedbackEndpoint != 0)
        setTransactionsPerMicroframe(address, playback.feedbackEndpoint, 1);
    setSampleRate(client, device, playback.endpoint);

    // Playback transfers. Each packet is one service interval (1 ms).
    const int packetsPerUrb =
        std::min(CONFIG_PROBE_PACKETS_PER_URB,
                 std::max(1, kUsableIsocDescriptors / playback.intervalMicroframes));
    const int packetBytes = playback.maxPacket * playback.transactions;
    stream.running.store(true, std::memory_order_release);
    for (int i = 0; i < CONFIG_PROBE_URBS_IN_FLIGHT; ++i) {
        usb_transfer_t* transfer = nullptr;
        if (usb_host_transfer_alloc(static_cast<std::size_t>(packetBytes * packetsPerUrb),
                                    packetsPerUrb, &transfer) != ESP_OK) {
            ESP_LOGE(kTag, "out of memory for playback transfers");
            break;
        }
        transfer->device_handle = device;
        transfer->bEndpointAddress = playback.endpoint;
        transfer->callback = &playbackDone;
        transfer->context = &stream;
        stream.transfers.push_back(transfer);
    }
    // Feedback transfers: whole packets, as many as the descriptor list holds.
    if (playback.feedbackEndpoint != 0 && playback.feedbackMaxPacket > 0 &&
        playback.feedbackIntervalMicroframes <= kUsableIsocDescriptors) {
        const int packets = std::max(1, kUsableIsocDescriptors / playback.feedbackIntervalMicroframes);
        if (usb_host_transfer_alloc(static_cast<std::size_t>(playback.feedbackMaxPacket * packets),
                                    packets, &stream.feedback) == ESP_OK) {
            stream.feedback->device_handle = device;
            stream.feedback->bEndpointAddress = playback.feedbackEndpoint;
            stream.feedback->callback = &feedbackDone;
            stream.feedback->context = &stream;
            stream.feedback->num_bytes = playback.feedbackMaxPacket * packets;
            for (int i = 0; i < packets; ++i)
                stream.feedback->isoc_packet_desc[i].num_bytes = playback.feedbackMaxPacket;
        }
    } else if (playback.feedbackEndpoint != 0) {
        ESP_LOGW(kTag, "feedback ep 0x%02x interval %d uframes does not fit one transfer; "
                       "streaming at nominal rate", playback.feedbackEndpoint,
                 playback.feedbackIntervalMicroframes);
    }

    if (stream.feedback != nullptr) {
        stream.inFlight.fetch_add(1, std::memory_order_acq_rel);
        if (const esp_err_t err = usb_host_transfer_submit(stream.feedback); err != ESP_OK) {
            stream.inFlight.fetch_sub(1, std::memory_order_acq_rel);
            ESP_LOGE(kTag, "feedback submit failed: %s", esp_err_to_name(err));
        }
    }
    for (usb_transfer_t* transfer : stream.transfers) {
        fillPlayback(stream, transfer);
        stream.inFlight.fetch_add(1, std::memory_order_acq_rel);
        if (const esp_err_t err = usb_host_transfer_submit(transfer); err != ESP_OK) {
            stream.inFlight.fetch_sub(1, std::memory_order_acq_rel);
            ESP_LOGE(kTag, "playback submit failed: %s", esp_err_to_name(err));
        }
    }
    ESP_LOGI(kTag, "streaming %d Hz at %d dBFS on channels 1-2: %d transfers x %d packets, "
                   "up to %d frames (%d bytes) per packet",
             kVariants[g_variant].toneHz, CONFIG_PROBE_TONE_DBFS, static_cast<int>(stream.transfers.size()),
             packetsPerUrb, playback.maxFramesPerPacket(), packetBytes);
    return true;
}

void stopStream(usb_host_client_handle_t client, bool deviceGone, bool closeDevice = true) {
    Stream& stream = g_stream;
    if (stream.device == nullptr) return;
    stream.running.store(false, std::memory_order_release);
    // Isochronous transfers retire on their own within a few milliseconds.
    const std::int64_t deadline = esp_timer_get_time() + 500'000;
    while (stream.inFlight.load(std::memory_order_acquire) > 0 && esp_timer_get_time() < deadline)
        vTaskDelay(pdMS_TO_TICKS(5));
    if (stream.inFlight.load(std::memory_order_acquire) > 0) {
        ESP_LOGW(kTag, "transfers still in flight; halting and flushing the endpoints");
        for (const std::uint8_t ep : {stream.playback.endpoint, stream.playback.feedbackEndpoint}) {
            if (ep == 0) continue;
            usb_host_endpoint_halt(stream.device, ep);
            usb_host_endpoint_flush(stream.device, ep);
        }
        while (stream.inFlight.load(std::memory_order_acquire) > 0) vTaskDelay(pdMS_TO_TICKS(5));
    }
    for (usb_transfer_t* transfer : stream.transfers) usb_host_transfer_free(transfer);
    stream.transfers.clear();
    if (stream.feedback != nullptr) usb_host_transfer_free(stream.feedback);
    stream.feedback = nullptr;
    if (!deviceGone) {
        // Alternate setting 0 has no endpoints: the TP-7 stops expecting audio.
        (void)setInterface(client, stream.device, stream.playback.interface, 0);
        usb_host_interface_release(client, stream.device, stream.playback.interface);
    }
    if (closeDevice || deviceGone) {
        usb_host_device_close(client, stream.device);
        stream.device = nullptr;
    }
}

/// Once a second: what the bus did since the last report.
void report(std::uint64_t& lastFrames, std::uint32_t& lastPackets, std::int64_t& lastTime) {
    Stats& stats = g_stream.stats;
    const std::int64_t now = esp_timer_get_time();
    const std::uint64_t frames = stats.frames.load(std::memory_order_relaxed);
    const std::uint32_t packets = stats.packets.load(std::memory_order_relaxed);
    const double seconds = static_cast<double>(now - lastTime) / 1e6;
    const std::uint32_t raw = stats.feedbackRaw.load(std::memory_order_relaxed);
    ESP_LOGI(kTag,
             "%" PRIu32 " packets/s, %.0f frames/s queued | bad packets %" PRIu32
             ", bad transfers %" PRIu32 " (last: %s) | feedback 0x%08" PRIx32
             " = %.4f frames/ms (%s), %" PRIu32 " reports, %" PRIu32 " errors (last: %s, %" PRIu32
             " bytes)",
             static_cast<std::uint32_t>((packets - lastPackets) / seconds),
             static_cast<double>(frames - lastFrames) / seconds,
             stats.badPackets.load(std::memory_order_relaxed),
             stats.badUrbs.load(std::memory_order_relaxed),
             probe::transferStatusName(static_cast<usb_transfer_status_t>(
                 stats.lastBadStatus.load(std::memory_order_relaxed))),
             raw, g_stream.rateQ16.load(std::memory_order_relaxed) / 65536.0,
             g_stream.feedbackFormat.load(std::memory_order_relaxed),
             stats.feedbackPackets.load(std::memory_order_relaxed),
             stats.feedbackErrors.load(std::memory_order_relaxed),
             probe::transferStatusName(static_cast<usb_transfer_status_t>(
                 stats.lastFeedbackStatus.load(std::memory_order_relaxed))),
             stats.lastFeedbackBytes.load(std::memory_order_relaxed));
    ESP_LOGI(kTag, "  completion gap %" PRIu32 "-%" PRIu32 " uframes, %" PRIu32 "-%" PRIu32
                   " frames per packet",
             stats.minGap.exchange(UINT32_MAX), stats.maxGap.exchange(0),
             stats.minFrames.exchange(UINT32_MAX), stats.maxFrames.exchange(0));
    lastFrames = frames;
    lastPackets = packets;
    lastTime = now;
}

// ---------------------------------------------------------------------- setup

bool openTp7(usb_host_client_handle_t client, std::uint8_t address) {
    usb_device_handle_t device = nullptr;
    if (const esp_err_t err = usb_host_device_open(client, address, &device); err != ESP_OK) {
        ESP_LOGE(kTag, "open device %u failed: %s", address, esp_err_to_name(err));
        return false;
    }
    usb_device_info_t info{};
    const usb_device_desc_t* deviceDescriptor = nullptr;
    const usb_config_desc_t* config = nullptr;
    usb_host_device_info(device, &info);
    usb_host_get_device_descriptor(device, &deviceDescriptor);
    usb_host_get_active_config_descriptor(device, &config);
    ESP_LOGI(kTag, "---- device %u attached: %04x:%04x, %s speed", address,
             deviceDescriptor->idVendor, deviceDescriptor->idProduct,
             info.speed == USB_SPEED_HIGH ? "high" : "NOT high");

    const bool tp7Audio = deviceDescriptor->idVendor == kTeenageEngineering &&
                          deviceDescriptor->idProduct != kTp7MtpProduct;
    const Playback playback = findPlayback(config);
    if (!tp7Audio || !playback.found || info.speed != USB_SPEED_HIGH) {
        ESP_LOGW(kTag, "not a high-speed TP-7 in USB-audio mode with a playback endpoint; ignored");
        usb_host_device_close(client, device);
        return false;
    }
    ESP_LOGI(kTag,
             "playback: interface %u alt %u, ep 0x%02x, %d ch x %d-bit in %d bytes, "
             "%u bytes x %d transactions every %d uframes; feedback ep 0x%02x (%u bytes every %d "
             "uframes)",
             playback.interface, playback.alternate, playback.endpoint, playback.channels,
             playback.bits, playback.subslot, playback.maxPacket, playback.transactions,
             playback.intervalMicroframes, playback.feedbackEndpoint, playback.feedbackMaxPacket,
             playback.feedbackIntervalMicroframes);
    if (playback.subslot < 3) {
        ESP_LOGE(kTag, "unexpected sample container (%d bytes)", playback.subslot);
        usb_host_device_close(client, device);
        return false;
    }
    g_address = address;
    if (!startStream(client, device, address, playback)) {
        usb_host_device_close(client, device);
        g_stream.device = nullptr;
        return false;
    }
    return true;
}

void hostLibraryTask(void* notify) {
    usb_host_config_t config{};
    config.intr_flags = ESP_INTR_FLAG_LEVEL1;
    config.peripheral_map = BIT0;  // The high-speed OTG controller.
    // The controller's FIFO is 896 lines of 4 bytes (GHWCFG3; 1024 less what
    // DMA mode reserves). The default split leaves the periodic TX FIFO 512
    // bytes, which rejects the TP-7's 882-byte endpoint. One microframe of its
    // playback is 1764 bytes, so periodic TX gets 448 lines (1792 bytes).
    constexpr std::uint32_t kFifoLines = 896;
    config.fifo_settings_custom.nptx_fifo_lines = 128;  // One 512-byte bulk packet.
    config.fifo_settings_custom.ptx_fifo_lines = 448;
    config.fifo_settings_custom.rx_fifo_lines = kFifoLines - 128 - 448;
    ESP_ERROR_CHECK(usb_host_install(&config));
    const std::uint32_t depth = USB_DWC_HS.ghwcfg3_reg.dfifodepth;
    if (depth != kFifoLines)
        ESP_LOGE(kTag, "controller FIFO is %" PRIu32 " lines, not %" PRIu32
                       "; the custom split will be rejected", depth, kFifoLines);
    ESP_LOGI(kTag, "USB host installed on the high-speed port; FIFO: RX %" PRIu32
                   ", non-periodic TX 128, periodic TX 448 lines",
             kFifoLines - 128 - 448);
    xTaskNotifyGive(static_cast<TaskHandle_t>(notify));
    for (;;) {
        std::uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
    }
}

void enableVbus() {
#if CONFIG_PROBE_VBUS_ENABLE_GPIO >= 0
    const auto pin = static_cast<gpio_num_t>(CONFIG_PROBE_VBUS_ENABLE_GPIO);
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 1);
#endif
}

}  // namespace

extern "C" void app_main(void) {
    enableVbus();
    xTaskCreatePinnedToCore(&hostLibraryTask, "usb_host", 4096, xTaskGetCurrentTaskHandle(), 6,
                            nullptr, 0);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    probe::UsbClient client;
    ESP_ERROR_CHECK(client.start());
    ESP_LOGI(kTag, "waiting for a TP-7 in USB-audio mode");

    std::int64_t startedAt = 0;
    std::uint64_t lastFrames = 0;
    std::uint32_t lastPackets = 0;
    std::int64_t lastTime = 0;
    for (;;) {
        probe::UsbEvent event;
        if (client.waitEvent(event, pdMS_TO_TICKS(1000))) {
            if (event.kind == probe::UsbEvent::Kind::Attached && g_stream.device == nullptr) {
                if (openTp7(client.handle(), event.address)) {
                    startedAt = lastTime = esp_timer_get_time();
                    lastFrames = 0;
                    lastPackets = 0;
                }
            } else if (event.kind == probe::UsbEvent::Kind::Detached &&
                       event.device == g_stream.device && g_stream.device != nullptr) {
                ESP_LOGW(kTag, "---- TP-7 detached while streaming");
                stopStream(client.handle(), true);
            }
            continue;
        }
        if (g_stream.device == nullptr) continue;
        report(lastFrames, lastPackets, lastTime);
        if (esp_timer_get_time() - startedAt >= std::int64_t{kVariantSeconds} * 1'000'000) {
            usb_device_handle_t device = g_stream.device;
            const Playback playback = g_stream.playback;
            stopStream(client.handle(), false, false);
            g_variant = (g_variant + 1) % static_cast<int>(std::size(kVariants));
            if (!startStream(client.handle(), device, g_address, playback)) {
                usb_host_device_close(client.handle(), device);
                g_stream.device = nullptr;
            }
            startedAt = lastTime = esp_timer_get_time();
            lastFrames = 0;
            lastPackets = 0;
            continue;
        }
        if (CONFIG_PROBE_SECONDS > 0 &&
            esp_timer_get_time() - startedAt >= std::int64_t{CONFIG_PROBE_SECONDS} * 1'000'000) {
            const Stats& stats = g_stream.stats;
            ESP_LOGI(kTag, "done after %d s: %" PRIu32 " packets, %" PRIu32 " bad, %" PRIu64
                           " frames",
                     CONFIG_PROBE_SECONDS, stats.packets.load(), stats.badPackets.load(),
                     stats.frames.load());
            stopStream(client.handle(), false);
            ESP_LOGI(kTag, "stopped; replug the TP-7 or reset to run again");
        }
    }
}
