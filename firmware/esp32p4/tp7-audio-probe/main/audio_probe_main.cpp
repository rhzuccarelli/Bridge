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

#include "PtpSession.hpp"
#include "Tp7Midi.hpp"
#include "UsbClient.hpp"
#include "esp_rom_crc.h"
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
    esp_err_t err = usb_host_transfer_alloc(USB_SETUP_PACKET_SIZE + std::max<std::size_t>(64, setup.wLength), 0,
                                             &transfer);
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

/// Logs every configuration descriptor the device offers, in hex, and the
/// configuration currently active. ESP-IDF always activates the first one;
/// the dump lets the three be compared off-line.
[[maybe_unused]] void dumpConfigurations(usb_host_client_handle_t client, usb_device_handle_t device,
                        int count) {
    usb_transfer_status_t status = USB_TRANSFER_STATUS_COMPLETED;
    usb_setup_packet_t setup{};
    setup.bmRequestType = 0x80;  // Device to host, standard, device.
    setup.bRequest = 0x08;       // GET_CONFIGURATION
    setup.wLength = 1;
    std::uint8_t active = 0;
    if (controlTransfer(client, device, setup, &active, status) == ESP_OK)
        ESP_LOGI(kTag, "DESC active configuration value %u of %d", active, count);
    // Control transfers are capped by CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE.
    constexpr std::size_t kMax = CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE - USB_SETUP_PACKET_SIZE;
    std::vector<std::uint8_t> bytes(kMax);
    for (int index = 0; index < count; ++index) {
        for (const std::size_t wanted : {std::size_t{9}, std::size_t{0}}) {
            const std::size_t length =
                wanted != 0 ? wanted : std::min<std::size_t>(kMax, bytes[2] | (bytes[3] << 8));
            setup = {};
            setup.bmRequestType = 0x80;
            setup.bRequest = 0x06;  // GET_DESCRIPTOR
            setup.wValue = static_cast<std::uint16_t>((0x02 << 8) | index);  // CONFIGURATION
            setup.wLength = static_cast<std::uint16_t>(length);
            if (controlTransfer(client, device, setup, bytes.data(), status) != ESP_OK) {
                ESP_LOGE(kTag, "DESC config %d: %s", index, probe::transferStatusName(status));
                break;
            }
            if (wanted != 0) continue;
            ESP_LOGI(kTag, "DESC config %d: %u bytes (wTotalLength %u)", index,
                     static_cast<unsigned>(length), bytes[2] | (bytes[3] << 8));
            for (std::size_t offset = 0; offset < length; offset += 32) {
                char line[3 * 32 + 1];
                int used = 0;
                for (std::size_t i = offset; i < std::min(length, offset + 32); ++i)
                    used += std::snprintf(line + used, sizeof(line) - static_cast<std::size_t>(used),
                                          "%02x", bytes[i]);
                ESP_LOGI(kTag, "DESC %d %04x %s", index, static_cast<unsigned>(offset), line);
            }
        }
    }
}

/// Reads the TP-7's master mute (feature unit 3 on audio-control interface 0,
/// from its descriptor) and clears it. The probe never touched it before; a
/// host such as macOS may leave the device muted.
void unmute(usb_host_client_handle_t client, usb_device_handle_t device) {
    constexpr std::uint16_t kFeatureUnit = 3;
    constexpr std::uint16_t kControlInterface = 0;
    usb_transfer_status_t status = USB_TRANSFER_STATUS_COMPLETED;
    usb_setup_packet_t setup{};
    setup.bmRequestType = 0xA1;  // Device to host, class, interface.
    setup.bRequest = 0x81;       // GET_CUR
    setup.wValue = 0x0100;       // MUTE_CONTROL, master channel
    setup.wIndex = (kFeatureUnit << 8) | kControlInterface;
    setup.wLength = 1;
    std::uint8_t muted = 0xFF;
    if (controlTransfer(client, device, setup, &muted, status) == ESP_OK)
        ESP_LOGI(kTag, "master mute was %u", muted);
    else
        ESP_LOGW(kTag, "GET_CUR mute: %s", probe::transferStatusName(status));
    setup.bmRequestType = 0x21;  // Host to device, class, interface.
    setup.bRequest = 0x01;       // SET_CUR
    std::uint8_t off = 0;
    if (controlTransfer(client, device, setup, &off, status) == ESP_OK)
        ESP_LOGI(kTag, "master mute cleared");
    else
        ESP_LOGW(kTag, "SET_CUR mute: %s", probe::transferStatusName(status));
}

// ----------------------------------------------------------------------- MIDI

/// The TP-7's USB-MIDI streaming interface (interface 3, bulk OUT 0x02 in its
/// descriptor), claimed alongside audio so the probe can try MIDI commands.
struct Midi {
    bool claimed{false};
    std::uint8_t interface{3};
    std::uint8_t endpoint{0x02};
};
Midi g_midi;

/// Sends one USB-MIDI event packet (4 bytes, cable 0) and waits for it.
bool sendMidiPacket(usb_device_handle_t device, const std::uint8_t (&packet)[4]) {
    if (!g_midi.claimed) return false;
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    usb_transfer_t* transfer = nullptr;
    bool ok = false;
    if (done != nullptr && usb_host_transfer_alloc(64, 0, &transfer) == ESP_OK) {
        std::memcpy(transfer->data_buffer, packet, sizeof packet);
        transfer->num_bytes = sizeof packet;
        transfer->device_handle = device;
        transfer->bEndpointAddress = g_midi.endpoint;
        transfer->callback = [](usb_transfer_t* t) {
            xSemaphoreGive(static_cast<SemaphoreHandle_t>(t->context));
        };
        transfer->context = done;
        if (usb_host_transfer_submit(transfer) == ESP_OK &&
            xSemaphoreTake(done, pdMS_TO_TICKS(1000)) == pdTRUE)
            ok = transfer->status == USB_TRANSFER_STATUS_COMPLETED;
        usb_host_transfer_free(transfer);
    }
    if (done != nullptr) vSemaphoreDelete(done);
    ESP_LOGW(kTag, "MIDI >>> %02x %02x %02x %02x: %s", packet[0], packet[1], packet[2], packet[3],
             ok ? "sent" : "FAILED");
    return ok;
}

/// A Control Change on channel 1 (CIN 0xB).
bool sendControlChange(usb_device_handle_t device, std::uint8_t controller, std::uint8_t value) {
    const std::uint8_t packet[4] = {0x0B, 0xB0, controller, value};
    return sendMidiPacket(device, packet);
}

// What the TP-7 sends back, read by one bulk IN transfer that resubmits
// itself. Pressing play makes it send Start (0xFA) and a clock (0xF8); stop
// sends Stop (0xFC). That confirms on the P4 whether it really started.
struct MidiIn {
    usb_transfer_t* transfer{nullptr};
    std::atomic<bool> running{false};
    std::atomic<bool> pending{false};
    std::atomic<std::uint32_t> starts{0};
    std::atomic<std::uint32_t> continues{0};
    std::atomic<std::uint32_t> stops{0};
    std::atomic<std::uint32_t> clocks{0};
    std::atomic<std::uint32_t> other{0};
    std::atomic<std::uint32_t> lastOther{0};
};
MidiIn g_midiIn;

void midiInDone(usb_transfer_t* transfer) {
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        for (int i = 0; i + 4 <= transfer->actual_num_bytes; i += 4) {
            const std::uint8_t* m = transfer->data_buffer + i;
            switch (m[1]) {
                case 0xF8: g_midiIn.clocks.fetch_add(1); break;
                case 0xFA: g_midiIn.starts.fetch_add(1); break;
                case 0xFB: g_midiIn.continues.fetch_add(1); break;
                case 0xFC: g_midiIn.stops.fetch_add(1); break;
                default:
                    if ((m[0] | m[1] | m[2] | m[3]) != 0) {
                        g_midiIn.other.fetch_add(1);
                        g_midiIn.lastOther.store((m[0] << 24) | (m[1] << 16) | (m[2] << 8) | m[3]);
                    }
            }
        }
    }
    if (g_midiIn.running.load() && transfer->status != USB_TRANSFER_STATUS_NO_DEVICE &&
        transfer->status != USB_TRANSFER_STATUS_CANCELED &&
        usb_host_transfer_submit(transfer) == ESP_OK)
        return;
    g_midiIn.pending.store(false);
}

void startMidiIn(usb_device_handle_t device) {
    if (g_midiIn.transfer == nullptr && usb_host_transfer_alloc(512, 0, &g_midiIn.transfer) != ESP_OK)
        return;
    g_midiIn.transfer->device_handle = device;
    g_midiIn.transfer->bEndpointAddress = 0x81;
    g_midiIn.transfer->num_bytes = 512;
    g_midiIn.transfer->callback = &midiInDone;
    g_midiIn.running.store(true);
    g_midiIn.pending.store(true);
    if (usb_host_transfer_submit(g_midiIn.transfer) != ESP_OK) g_midiIn.pending.store(false);
}

/// Cancels the pending read; call before releasing the MIDI interface.
void stopMidiIn(usb_device_handle_t device, bool deviceGone) {
    g_midiIn.running.store(false);
    if (g_midiIn.pending.load() && !deviceGone) {
        usb_host_endpoint_halt(device, 0x81);
        usb_host_endpoint_flush(device, 0x81);
    }
    for (int i = 0; i < 100 && g_midiIn.pending.load(); ++i) vTaskDelay(pdMS_TO_TICKS(5));
    if (!deviceGone) usb_host_endpoint_clear(device, 0x81);
}

void logMidiIn(const char* when) {
    ESP_LOGW(kTag, "MIDI <<< %s: Start %" PRIu32 ", Continue %" PRIu32 ", Stop %" PRIu32
                   ", clock %" PRIu32 ", other %" PRIu32 " (last %08" PRIx32 ")",
             when, g_midiIn.starts.load(), g_midiIn.continues.load(), g_midiIn.stops.load(),
             g_midiIn.clocks.load(), g_midiIn.other.load(), g_midiIn.lastOther.load());
}

/// Lab mode: each cycle plays one variant into the TP-7 and records it there.
/// Verified on the bench: CC 22 arms recording, CC 23 (play) starts it, CC 24
/// stops. Then the TP-7 is switched to MTP, the newest take is sent to the
/// Mac over the console as base64, and the session is closed, which returns
/// the TP-7 to USB audio for the next variant.
enum class LabState { Idle, Streaming, AwaitingMtp };
LabState g_lab = LabState::Idle;
/// MIDI sent during a lab cycle, in milliseconds after the TP-7 attaches.
/// Value 0 after 127 is the button release. The sequence that records on the
/// TP-7: rec (arms), a couple of seconds later play (recording runs), stop.
struct LabStep {
    int ms;
    std::uint8_t packet[4];  ///< USB-MIDI event packet.
};
/// Pressing play and stop on the TP-7 makes it send MIDI Start (0xFA) and Stop
/// (0xFC), not the CCs in the guide's controller-mode table, and it obeys a
/// Start and Stop sent to it. CC 22 did not arm recording, so each lab run
/// tries one way of arming; a new dated file on the TP-7 means it worked.
struct LabSequence {
    const char* name;
    LabStep steps[6];
    int count;
};
constexpr LabSequence kLabSequences[] = {
    {"CC 14 on, Start, Stop, CC 14 off",
     {{6000, {0x0B, 0xB0, 14, 127}}, {9000, {0x0F, 0xFA, 0, 0}}, {14000, {0x0F, 0xFC, 0, 0}},
      {15000, {0x0B, 0xB0, 14, 0}}},
     4},
};
constexpr int kLabMtpMs = 20000;  ///< Switch to MTP (leaves the TP-7 time to save).
int g_labRun = 0;

/// Sends the SysEx that makes the TP-7 re-enumerate as an MTP device.
void requestMtp(usb_device_handle_t device) {
    probe::BulkPipe out{device, g_midi.endpoint, 512};
    if (out.init(64) == ESP_OK && probe::sendTp7EnterMtp(out) == ESP_OK)
        ESP_LOGW(kTag, "LAB asked the TP-7 for MTP mode");
    else
        ESP_LOGE(kTag, "LAB MTP SysEx failed");
}

// ------------------------------------------------------- base64 over console

/// Streams bytes to the console as base64 lines ("B64 <data>"), 57 bytes per
/// line, keeping a CRC-32 of the raw bytes so the Mac can check the copy.
struct Base64Dump {
    std::uint8_t pending[57];
    std::size_t used{0};
    std::uint32_t crc{0};
    std::size_t total{0};
    std::size_t limit{0};

    void flushLine() {
        static constexpr char kAlphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        char line[4 + 76 + 2];
        int at = 0;
        line[at++] = 'B';
        line[at++] = '6';
        line[at++] = '4';
        line[at++] = ' ';
        for (std::size_t i = 0; i < used; i += 3) {
            const std::uint32_t b0 = pending[i];
            const std::uint32_t b1 = i + 1 < used ? pending[i + 1] : 0;
            const std::uint32_t b2 = i + 2 < used ? pending[i + 2] : 0;
            const std::uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
            line[at++] = kAlphabet[(triple >> 18) & 63];
            line[at++] = kAlphabet[(triple >> 12) & 63];
            line[at++] = i + 1 < used ? kAlphabet[(triple >> 6) & 63] : '=';
            line[at++] = i + 2 < used ? kAlphabet[triple & 63] : '=';
        }
        line[at++] = '\n';
        fwrite(line, 1, static_cast<std::size_t>(at), stdout);
        used = 0;
    }

    bool add(std::span<const std::uint8_t> bytes) {
        for (const std::uint8_t byte : bytes) {
            if (total >= limit) return false;
            pending[used++] = byte;
            ++total;
            if (used == sizeof pending) flushLine();
        }
        crc = esp_rom_crc32_le(crc, bytes.data(), static_cast<std::uint32_t>(bytes.size()));
        return true;
    }
};

bool dumpChunk(std::span<const std::uint8_t> chunk, void* context) {
    return static_cast<Base64Dump*>(context)->add(chunk);
}

// ------------------------------------------------------------- MTP fetching

struct MtpLayout {
    bool found{false};
    std::uint8_t interface{0};
    std::uint8_t alternate{0};
    std::uint8_t in{0};
    std::uint8_t out{0};
    std::uint16_t inMaxPacket{0};
    std::uint16_t outMaxPacket{0};
};

/// The Still Image (or vendor) interface with a bulk IN and a bulk OUT.
MtpLayout findMtp(const usb_config_desc_t* config) {
    MtpLayout layout;
    MtpLayout current;
    bool candidate = false;
    int offset = 0;
    const auto* descriptor = reinterpret_cast<const usb_standard_desc_t*>(config);
    while ((descriptor = usb_parse_next_descriptor(descriptor, config->wTotalLength, &offset))) {
        if (descriptor->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            if (candidate && current.in != 0 && current.out != 0 && !layout.found) {
                layout = current;
                layout.found = true;
            }
            const auto* intf = reinterpret_cast<const usb_intf_desc_t*>(descriptor);
            candidate = intf->bInterfaceClass == 0x06 || intf->bInterfaceClass == 0xFF;
            current = {};
            current.interface = intf->bInterfaceNumber;
            current.alternate = intf->bAlternateSetting;
        } else if (descriptor->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && candidate) {
            const auto* ep = reinterpret_cast<const usb_ep_desc_t*>(descriptor);
            if ((ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) != USB_BM_ATTRIBUTES_XFER_BULK)
                continue;
            const auto mps = static_cast<std::uint16_t>(ep->wMaxPacketSize & 0x7FF);
            if (ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) {
                current.in = ep->bEndpointAddress;
                current.inMaxPacket = mps;
            } else {
                current.out = ep->bEndpointAddress;
                current.outMaxPacket = mps;
            }
        }
    }
    if (candidate && current.in != 0 && current.out != 0 && !layout.found) {
        layout = current;
        layout.found = true;
    }
    return layout;
}

struct MtpObject {
    std::uint32_t handle{0};
    std::string name;
    std::uint32_t size{0};
    bool folder{false};
    std::string created;   ///< PTP date "YYYYMMDDThhmmss", sorts by time.
    std::string modified;
};

bool mtpObjectInfo(probe::PtpSession& session, std::uint32_t handle, MtpObject& object) {
    std::vector<std::uint8_t> data;
    probe::PtpResponse response;
    if (session.transact(probe::ptp::kGetObjectInfo, {handle}, data, response) != ESP_OK ||
        response.code != probe::ptp::kResponseOk)
        return false;
    probe::DatasetReader reader{data};
    reader.u32();                                   // StorageID
    const std::uint16_t format = reader.u16();
    reader.u16();                                   // ProtectionStatus
    const std::uint32_t size = reader.u32();
    reader.skip(2 + 4 * 6);                         // Thumb/Image fields
    reader.u32();                                   // ParentObject
    reader.u16();                                   // AssociationType
    reader.u32();                                   // AssociationDesc
    reader.u32();                                   // SequenceNumber
    object = {handle, reader.string(), size, format == probe::ptp::kFormatAssociation, {}, {}};
    object.created = reader.string();
    object.modified = reader.string();
    return reader.ok();
}

std::vector<MtpObject> mtpChildren(probe::PtpSession& session, std::uint32_t storage,
                                   std::uint32_t parent) {
    std::vector<MtpObject> children;
    std::vector<std::uint8_t> data;
    probe::PtpResponse response;
    if (session.transact(probe::ptp::kGetObjectHandles, {storage, 0, parent}, data, response) !=
            ESP_OK ||
        response.code != probe::ptp::kResponseOk)
        return children;
    for (const std::uint32_t handle : probe::DatasetReader{data}.u32Array()) {
        MtpObject object;
        if (mtpObjectInfo(session, handle, object)) children.push_back(std::move(object));
    }
    return children;
}

/// Walks the storage (three folder levels deep), collecting every file with
/// its path.
void collectFiles(probe::PtpSession& session, std::uint32_t storage, std::uint32_t parent,
                  const std::string& prefix, int depth, std::vector<MtpObject>& files) {
    for (MtpObject entry : mtpChildren(session, storage, parent)) {
        entry.name = prefix + entry.name;
        if (!entry.folder) {
            files.push_back(std::move(entry));
        } else if (depth > 0) {
            collectFiles(session, storage, entry.handle, entry.name + "/", depth - 1, files);
        }
    }
}

/// The most recently created file anywhere on the storage. Takes started over
/// MIDI did not show up in "recordings", so nothing is assumed about where
/// they land, or their names ("ZYLK 1H3.WAV").
bool newestTake(probe::PtpSession& session, std::uint32_t storage, MtpObject& take) {
    std::vector<MtpObject> files;
    collectFiles(session, storage, probe::ptp::kRootParent, "", 3, files);
    std::sort(files.begin(), files.end(), [](const MtpObject& a, const MtpObject& b) {
        if (a.created != b.created) return a.created > b.created;
        if (a.modified != b.modified) return a.modified > b.modified;
        return a.handle > b.handle;
    });
    ESP_LOGI(kTag, "LAB %u files on the TP-7; newest:", static_cast<unsigned>(files.size()));
    for (std::size_t i = 0; i < files.size() && i < 8; ++i)
        ESP_LOGI(kTag, "LAB   %-40s %10" PRIu32 " created %s modified %s", files[i].name.c_str(),
                 files[i].size, files[i].created.c_str(), files[i].modified.c_str());
    if (files.empty()) return false;
    take = files.front();
    return true;
}

/// Sends the newest take to the console, framed for the Mac-side collector.
void fetchNewestTake(usb_host_client_handle_t client, usb_device_handle_t device,
                     const MtpLayout& mtp, int variant) {
    if (usb_host_interface_claim(client, device, mtp.interface, mtp.alternate) != ESP_OK) {
        ESP_LOGE(kTag, "LAB cannot claim MTP interface");
        return;
    }
    // A previous run may have been cut off mid-transfer (a reset of the P4
    // while the TP-7 stayed in MTP mode), which leaves the TP-7 answering
    // nothing. The Still Image class Device Reset request clears that.
    {
        usb_setup_packet_t setup{};
        setup.bmRequestType = 0x21;  // Host to device, class, interface.
        setup.bRequest = 0x66;       // DEVICE_RESET_REQUEST
        setup.wIndex = mtp.interface;
        usb_transfer_status_t status = USB_TRANSFER_STATUS_COMPLETED;
        std::uint8_t none = 0;
        ESP_LOGI(kTag, "LAB Still Image device reset: %s",
                 controlTransfer(client, device, setup, &none, status) == ESP_OK
                     ? "ok"
                     : probe::transferStatusName(status));
    }
    {
        probe::BulkPipe out{device, mtp.out, mtp.outMaxPacket};
        probe::BulkPipe in{device, mtp.in, mtp.inMaxPacket};
        probe::PtpSession session{out, in};
        if (out.init(512) == ESP_OK && in.init(64 * 1024) == ESP_OK && session.open() == ESP_OK) {
            std::vector<std::uint8_t> data;
            probe::PtpResponse response;
            std::vector<std::uint32_t> storages;
            if (session.transact(probe::ptp::kGetStorageIds, {}, data, response) == ESP_OK &&
                response.code == probe::ptp::kResponseOk)
                storages = probe::DatasetReader{data}.u32Array();
            MtpObject take;
            if (!storages.empty() && newestTake(session, storages.front(), take)) {
                Base64Dump dump;
                dump.limit = std::min<std::size_t>(take.size, 2u << 20);
                printf("\nLABTAKE %d %d %u %s\n", g_labRun, variant,
                       static_cast<unsigned>(dump.limit), take.name.c_str());
                constexpr std::uint32_t kChunk = 256 * 1024;
                bool ok = true;
                for (std::size_t offset = 0; ok && offset < dump.limit; offset += kChunk) {
                    const auto wanted = static_cast<std::uint32_t>(
                        std::min<std::size_t>(kChunk, dump.limit - offset));
                    const esp_err_t err = session.transact(
                        probe::ptp::kGetPartialObject,
                        {take.handle, static_cast<std::uint32_t>(offset), wanted}, &dumpChunk, &dump,
                        response, 10000);
                    ok = err == ESP_OK && response.code == probe::ptp::kResponseOk;
                    if (!ok)
                        printf("\nLAB GetPartialObject at %u: %s, response 0x%04x\n",
                               static_cast<unsigned>(offset), esp_err_to_name(err), response.code);
                }
                if (dump.used > 0) dump.flushLine();
                printf("LABEND %s %u %08" PRIx32 "\n", ok ? "ok" : "failed",
                       static_cast<unsigned>(dump.total), dump.crc);
                fflush(stdout);
            } else {
                ESP_LOGE(kTag, "LAB no take found");
            }
            (void)session.close();
        } else {
            ESP_LOGE(kTag, "LAB MTP session failed");
        }
    }
    usb_host_interface_release(client, device, mtp.interface);
}

// ------------------------------------------------------- host channel registers

/// How packets are sized.
enum class Sizing {
    Feedback,    ///< Exactly what the feedback asks for.
    Alternate1,  ///< 95, 97, 95, 97...: varies by 1, ignores the feedback.
    Alternate2,  ///< 94, 98, 94, 98...: varies by 2, always aligned.
    Fixed,       ///< 96 frames every packet, ignores the feedback.
    TP7Rate,     ///< A fixed rate from the variant (rateMilli), ignoring the feedback.
};

/// One way of streaming. The probe cycles through these, each with its own
/// tone. A recording made on the TP-7 showed it accepts almost none of the
/// data: short bursts of tone between long digital silence. The suspect is
/// the data PID. ESP-IDF stops and restarts the channel for every transfer
/// and never resets HCTSIZ.PID for isochronous pipes, so every transfer after
/// the first starts with whatever PID the core left (DATA2 was seen). With a
/// single transfer in flight the channel is idle between transfers, and the
/// probe can put MDATA back before each one.
struct Variant {
    const char* name;
    int toneHz;
    Sizing sizing;
    int transfers;      ///< Kept in flight.
    int packets;        ///< 1 ms packets per transfer.
    /// HCTSIZ.PID written before every submit (only safe with 1 transfer):
    /// 0 DATA0, 1 DATA2, 2 DATA1, 3 MDATA.
    std::uint32_t pid;
    int transactions;   ///< HCCHAR.EC: transactions per microframe.
    int frames;         ///< Frames per packet (Sizing::Fixed).
    /// Instead of a sine, each 1 ms packet carries a level that numbers it
    /// (packet index mod 16), positive in the first transaction's 49 frames
    /// and negative in the rest, so a recording shows which packets and
    /// which halves arrived.
    bool coded{false};
    int dbfs{-18};      ///< Sine level, or the coded signal's peak.
    bool square{false}; ///< A square wave at the tone frequency instead of a sine.
    /// A ramp numbering every 96 kHz frame: value = (n mod 8192) / 8192 - 0.5,
    /// times 0.5. Each frame is unique within 85 ms, so a recording shows
    /// missing, repeated or reordered frames anywhere.
    bool ramp{false};
    int rateMilli{96000};  ///< Sizing::TP7Rate: frames per ms, times 1000.
};
/// Fifth pass. A sweep of all four starting PIDs played nothing, while the
/// TP-7 plays USB audio from a Mac. This splits "the TP-7 accepts nothing
/// from the P4" from "the two-transaction microframe is what fails": the
/// first variant sends one plain 882-byte transaction per millisecond (49
/// frames, half of what the TP-7 consumes, so it will be choppy).
/// A frame-numbering ramp showed every frame arrives intact and in order, but
/// the TP-7 mutes 8-11 ms stretches and drops 2 frames in each: overflow
/// slips, about 57 frames/s, while following a feedback stuck at 96.000.
/// This sweeps fixed rates to find the TP-7's real one.
constexpr Variant kVariants[] = {
    {"RAMP at 95.900 frames/ms", 660, Sizing::TP7Rate, 3, 4, 3, 2, 96, false, -18, false, true, 95900},
    {"RAMP at 95.940 frames/ms", 660, Sizing::TP7Rate, 3, 4, 3, 2, 96, false, -18, false, true, 95940},
    {"RAMP at 95.970 frames/ms", 660, Sizing::TP7Rate, 3, 4, 3, 2, 96, false, -18, false, true, 95970},
    {"RAMP at 96.000 frames/ms", 660, Sizing::TP7Rate, 3, 4, 3, 2, 96, false, -18, false, true, 96000},
};
constexpr int kVariantSeconds = 8;
int g_variant = 0;
std::uint8_t g_address = 0;
int g_playbackChannel = -1;  ///< DWC host channel of the playback endpoint.

/// HCTSIZ.PID encodings.
constexpr std::uint32_t kPidData0 = 0;
constexpr std::uint32_t kPidData2 = 1;
constexpr std::uint32_t kPidData1 = 2;
constexpr std::uint32_t kPidMData = 3;

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
    std::atomic<std::uint32_t> oddPackets{0};  ///< Odd frame count: next packet misaligned.
    /// Feedback values seen since the last report: how much the TP-7 jitters.
    std::atomic<std::uint32_t> feedbackMin{UINT32_MAX};
    std::atomic<std::uint32_t> feedbackMax{0};
    /// Bytes the controller reports as actually sent per OUT packet (requested
    /// minus what DMA left over). A status of "completed" alone does not show
    /// whether the second transaction of a microframe went out.
    std::atomic<std::uint32_t> sentMin{UINT32_MAX};
    std::atomic<std::uint32_t> sentMax{0};
    std::atomic<std::uint32_t> shortPackets{0};
    std::atomic<std::uint32_t> feedbackTransfers{0};
    std::atomic<std::uint32_t> feedbackEmpty{0};  ///< Completed with under 3 bytes.
    /// First bytes of an "empty" feedback packet. The buffer is filled with
    /// 0xEE before each submit, so anything else means data did arrive and
    /// only the reported length is wrong.
    std::atomic<std::uint32_t> emptyPacketBytes{0xEEEEEEEE};
    std::atomic<std::uint32_t> lastFeedbackStatus{USB_TRANSFER_STATUS_COMPLETED};
    std::atomic<std::uint32_t> lastFeedbackBytes{0};
    /// Microframes between playback completions since the last report.
    std::atomic<std::uint32_t> minGap{UINT32_MAX};
    std::atomic<std::uint32_t> maxGap{0};
    std::atomic<std::uint32_t> minFrames{UINT32_MAX};
    std::atomic<std::uint32_t> maxFrames{0};

    void reset() {
        for (auto* counter : {&urbs, &packets, &badPackets, &badUrbs, &feedbackRaw,
                              &feedbackPackets, &feedbackErrors, &oddPackets, &shortPackets,
                              &feedbackTransfers, &feedbackEmpty})
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
    std::uint32_t packetIndex{0};
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
        const int nominal = static_cast<int>(kSampleRate / 1000);
        const int sign = (stream.packetIndex++ & 1) != 0 ? 1 : -1;
        switch (kVariants[g_variant].sizing) {
            case Sizing::Feedback: break;
            case Sizing::Alternate1: frames = nominal + sign; break;
            case Sizing::Alternate2: frames = nominal + 2 * sign; break;
            case Sizing::Fixed: frames = kVariants[g_variant].frames; break;
            case Sizing::TP7Rate: break;  // Accumulated below at the fixed rate.
        }
        if (kVariants[g_variant].sizing == Sizing::TP7Rate) {
            const auto rateQ16 = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(kVariants[g_variant].rateMilli) << 16) / 1000);
            const std::uint32_t owed = stream.remainderQ16 + rateQ16;
            frames = static_cast<int>(owed >> 16);
            stream.remainderQ16 = owed - (static_cast<std::uint32_t>(frames) << 16);
        }
        frames = std::clamp(frames, 1, maxFrames);
        // What was not sent this packet is owed to the next one.
        if (kVariants[g_variant].sizing == Sizing::Feedback)
            stream.remainderQ16 = due - (static_cast<std::uint32_t>(frames) << 16);
        else if (kVariants[g_variant].sizing != Sizing::TP7Rate)
            stream.remainderQ16 = 0;
        if ((frames & 1) != 0) stream.stats.oddPackets.fetch_add(1, std::memory_order_relaxed);
        for (int frame = 0; frame < frames; ++frame) {
            const float wave = static_cast<float>(std::sin(stream.phase));
            float value = stream.amplitude *
                          (kVariants[g_variant].square ? (wave >= 0.0F ? 1.0F : -1.0F) : wave);
            if (kVariants[g_variant].ramp) {
                const auto n = static_cast<std::uint32_t>(stream.stats.frames.load(
                                   std::memory_order_relaxed)) + static_cast<std::uint32_t>(frame);
                value = 0.5F * (static_cast<float>(n % 8192) / 8192.0F - 0.5F);
            }
            if (kVariants[g_variant].coded) {
                // Packet k: level 0.05 + 0.05 * (k mod 16), sign by transaction.
                const auto k = static_cast<int>((stream.packetIndex - 1) & 15);
                const float level = stream.amplitude * (0.05F + 0.05F * static_cast<float>(k)) / 0.8F;
                const int firstTransactionFrames = p.maxPacket / frameBytes;  // 882 / 18 = 49
                value = frame < firstTransactionFrames ? level : -level;
            }
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

/// Puts the playback channel's PID back to the variant's, counting what the core left
/// there. Only while the channel is idle: with one transfer in flight, the
/// completion callback is such a moment.
std::atomic<std::uint32_t> g_pidLeft[4];
void restartPid() {
    if (g_playbackChannel < 0) return;
    volatile usb_dwc_host_chan_regs_t& channel = USB_DWC_HS.host_chans[g_playbackChannel];
    usb_dwc_hctsiz_reg_t hctsiz;
    hctsiz.val = channel.hctsiz_reg.val;
    g_pidLeft[hctsiz.pid & 3].fetch_add(1, std::memory_order_relaxed);
    hctsiz.pid = kVariants[g_variant].pid;
    channel.hctsiz_reg.val = hctsiz.val;
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
        const auto sent = static_cast<std::uint32_t>(desc.actual_num_bytes);
        if (sent < stats.sentMin.load(std::memory_order_relaxed))
            stats.sentMin.store(sent, std::memory_order_relaxed);
        if (sent > stats.sentMax.load(std::memory_order_relaxed))
            stats.sentMax.store(sent, std::memory_order_relaxed);
        if (desc.actual_num_bytes < desc.num_bytes)
            stats.shortPackets.fetch_add(1, std::memory_order_relaxed);
        if (desc.status != USB_TRANSFER_STATUS_COMPLETED) {
            stats.badPackets.fetch_add(1, std::memory_order_relaxed);
            stats.lastBadStatus.store(desc.status, std::memory_order_relaxed);
        }
    }
    if (stream.running.load(std::memory_order_acquire) &&
        transfer->status != USB_TRANSFER_STATUS_NO_DEVICE) {
        fillPlayback(stream, transfer);
        if (kVariants[g_variant].transfers == 1) restartPid();
        if (usb_host_transfer_submit(transfer) == ESP_OK) return;
    }
    stream.inFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void feedbackDone(usb_transfer_t* transfer) {
    Stream& stream = *static_cast<Stream*>(transfer->context);
    const int stride = stream.playback.feedbackMaxPacket;
    stream.stats.feedbackTransfers.fetch_add(1, std::memory_order_relaxed);
    for (int packet = 0; packet < transfer->num_isoc_packets; ++packet) {
        const usb_isoc_packet_desc_t& desc = transfer->isoc_packet_desc[packet];
        stream.stats.lastFeedbackBytes.store(static_cast<std::uint32_t>(desc.actual_num_bytes),
                                             std::memory_order_relaxed);
        if (desc.status != USB_TRANSFER_STATUS_COMPLETED || desc.actual_num_bytes < 3) {
            if (desc.status == USB_TRANSFER_STATUS_COMPLETED) {
                stream.stats.feedbackEmpty.fetch_add(1, std::memory_order_relaxed);
                const std::uint8_t* b = transfer->data_buffer + packet * stride;
                stream.stats.emptyPacketBytes.store(
                    b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<std::uint32_t>(b[3]) << 24),
                    std::memory_order_relaxed);
            }
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
        if (raw < stream.stats.feedbackMin.load(std::memory_order_relaxed))
            stream.stats.feedbackMin.store(raw, std::memory_order_relaxed);
        if (raw > stream.stats.feedbackMax.load(std::memory_order_relaxed))
            stream.stats.feedbackMax.store(raw, std::memory_order_relaxed);
        stream.stats.feedbackPackets.fetch_add(1, std::memory_order_relaxed);
        applyFeedback(stream, raw);
    }
    std::memset(transfer->data_buffer, 0xEE, static_cast<std::size_t>(transfer->num_bytes));
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
    stream.packetIndex = 0;
    stream.lastCompletion = -1;
    stream.phase = 0.0;
    stream.phaseStep = 2.0 * M_PI * kVariants[g_variant].toneHz / kSampleRate;
    ESP_LOGW(kTag, "==== variant %d: %s -> %d Hz tone", g_variant + 1, kVariants[g_variant].name,
             kVariants[g_variant].toneHz);
    stream.amplitude = static_cast<float>(std::pow(10.0, kVariants[g_variant].dbfs / 20.0));

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
        g_playbackChannel = setTransactionsPerMicroframe(address, playback.endpoint,
                                                         kVariants[g_variant].transactions);
        if (g_playbackChannel < 0)
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
        std::min(kVariants[g_variant].packets,
                 std::max(1, kUsableIsocDescriptors / playback.intervalMicroframes));
    const int packetBytes = playback.maxPacket * playback.transactions;
    stream.running.store(true, std::memory_order_release);
    for (int i = 0; i < kVariants[g_variant].transfers; ++i) {
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
            std::memset(stream.feedback->data_buffer, 0xEE,
                        static_cast<std::size_t>(stream.feedback->num_bytes));
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
             " bytes) | odd-sized packets %" PRIu32,
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
             stats.lastFeedbackBytes.load(std::memory_order_relaxed),
             stats.oddPackets.load(std::memory_order_relaxed));
    ESP_LOGI(kTag, "  completion gap %" PRIu32 "-%" PRIu32 " uframes, %" PRIu32 "-%" PRIu32
                   " frames per packet, feedback 0x%06" PRIx32 "-0x%06" PRIx32,
             stats.minGap.exchange(UINT32_MAX), stats.maxGap.exchange(0),
             stats.minFrames.exchange(UINT32_MAX), stats.maxFrames.exchange(0),
             stats.feedbackMin.exchange(UINT32_MAX), stats.feedbackMax.exchange(0));
    std::uint32_t ec = 0;
    if (g_playbackChannel >= 0) {
        usb_dwc_hcchar_reg_t hcchar;
        hcchar.val = USB_DWC_HS.host_chans[g_playbackChannel].hcchar_reg.val;
        ec = hcchar.ec;
    }
    ESP_LOGI(kTag, "  sent %" PRIu32 "-%" PRIu32 " bytes per packet, %" PRIu32
                   " short packets | playback channel EC now %" PRIu32 " | feedback: %" PRIu32
                   " transfers back, %" PRIu32 " empty packets (buffer 0x%08" PRIx32 ")"
                   " | PID left at restart: DATA0 %" PRIu32 ", DATA2 %" PRIu32 ", DATA1 %" PRIu32
                   ", MDATA %" PRIu32,
             stats.sentMin.exchange(UINT32_MAX), stats.sentMax.exchange(0),
             stats.shortPackets.load(std::memory_order_relaxed), ec,
             stats.feedbackTransfers.load(std::memory_order_relaxed),
             stats.feedbackEmpty.load(std::memory_order_relaxed),
             stats.emptyPacketBytes.load(std::memory_order_relaxed), g_pidLeft[0].exchange(0),
             g_pidLeft[1].exchange(0), g_pidLeft[2].exchange(0), g_pidLeft[3].exchange(0));
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
    unmute(client, device);
    if (const esp_err_t err = usb_host_interface_claim(client, device, g_midi.interface, 0);
        err == ESP_OK) {
        g_midi.claimed = true;
        ESP_LOGI(kTag, "claimed MIDI interface %u", g_midi.interface);
        startMidiIn(device);
    } else {
        ESP_LOGE(kTag, "claim MIDI interface: %s", esp_err_to_name(err));
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

/// MIDI listen mode: no audio, no MTP. Claims the TP-7's MIDI interface and
/// prints every USB-MIDI event packet the TP-7 sends (bulk IN 0x81), so its
/// own messages for rec, play and stop can be read off and replayed.
constexpr bool kMidiListen = false;
/// Fetch-only mode: when a TP-7 attaches in audio mode, switch it straight to
/// MTP and fetch its newest take, without streaming or recording anything.
/// For takes made elsewhere (for example a sine played from a Mac).
constexpr bool kFetchOnly = false;

void listenMidi(usb_host_client_handle_t client, std::uint8_t address) {
    usb_device_handle_t device = nullptr;
    if (usb_host_device_open(client, address, &device) != ESP_OK) return;
    const usb_device_desc_t* descriptor = nullptr;
    usb_host_get_device_descriptor(device, &descriptor);
    ESP_LOGW(kTag, "MIDI LISTEN: device %04x:%04x attached; press buttons on the TP-7",
             descriptor->idVendor, descriptor->idProduct);
    if (usb_host_interface_claim(client, device, 3, 0) != ESP_OK) {
        ESP_LOGE(kTag, "MIDI LISTEN: cannot claim MIDI interface 3");
        usb_host_device_close(client, device);
        return;
    }
    {
        probe::BulkPipe in{device, 0x81, 512};
        if (in.init(512) == ESP_OK) {
            for (;;) {
                std::size_t received = 0;
                const esp_err_t err = in.read(512, received, 1000);
                if (err == ESP_ERR_TIMEOUT) continue;
                if (err != ESP_OK) {
                    ESP_LOGW(kTag, "MIDI LISTEN: read ended (%s)", esp_err_to_name(err));
                    break;
                }
                for (std::size_t i = 0; i + 4 <= received; i += 4) {
                    const std::uint8_t* m = in.data() + i;
                    if ((m[0] | m[1] | m[2] | m[3]) == 0) continue;
                    ESP_LOGW(kTag, "MIDI IN  %02x %02x %02x %02x  (status %02x, data %u %u)", m[0],
                             m[1], m[2], m[3], m[1], m[2], m[3]);
                }
            }
        }
    }
    usb_host_interface_release(client, device, 3);
    usb_host_device_close(client, device);
}

extern "C" void app_main(void) {
    enableVbus();
    xTaskCreatePinnedToCore(&hostLibraryTask, "usb_host", 4096, xTaskGetCurrentTaskHandle(), 6,
                            nullptr, 0);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    probe::UsbClient client;
    ESP_ERROR_CHECK(client.start());
    ESP_LOGI(kTag, "waiting for a TP-7 in USB-audio mode");

    std::int64_t attachedAt = 0;
    std::uint64_t lastFrames = 0;
    std::uint32_t lastPackets = 0;
    std::int64_t lastTime = 0;
    int labStep = 0;
    for (;;) {
        probe::UsbEvent event;
        if (client.waitEvent(event, pdMS_TO_TICKS(200))) {
            if (kMidiListen && event.kind == probe::UsbEvent::Kind::Attached) {
                listenMidi(client.handle(), event.address);
                continue;
            }
            if (event.kind == probe::UsbEvent::Kind::Attached && g_stream.device == nullptr) {
                // An MTP TP-7 is the take we asked for; anything else may be audio.
                usb_device_handle_t device = nullptr;
                bool mtp = false;
                if (usb_host_device_open(client.handle(), event.address, &device) == ESP_OK) {
                    const usb_device_desc_t* descriptor = nullptr;
                    const usb_config_desc_t* config = nullptr;
                    usb_host_get_device_descriptor(device, &descriptor);
                    usb_host_get_active_config_descriptor(device, &config);
                    mtp = descriptor->idVendor == kTeenageEngineering &&
                          descriptor->idProduct == kTp7MtpProduct;
                    if (mtp) {
                        ESP_LOGW(kTag, "LAB TP-7 is in MTP mode; fetching the newest take");
                        const MtpLayout layout = findMtp(config);
                        if (layout.found) fetchNewestTake(client.handle(), device, layout, g_variant);
                        g_variant = (g_variant + 1) % static_cast<int>(std::size(kVariants));
                        ++g_labRun;
                        g_lab = LabState::Idle;
                    }
                    usb_host_device_close(client.handle(), device);
                }
                if (!mtp && kFetchOnly) {
                    if (usb_host_device_open(client.handle(), event.address, &device) == ESP_OK) {
                        if (usb_host_interface_claim(client.handle(), device, g_midi.interface, 0) ==
                            ESP_OK) {
                            g_midi.claimed = true;
                            requestMtp(device);
                            usb_host_interface_release(client.handle(), device, g_midi.interface);
                            g_midi.claimed = false;
                        }
                        usb_host_device_close(client.handle(), device);
                    }
                    continue;
                }
                if (!mtp && openTp7(client.handle(), event.address)) {
                    attachedAt = lastTime = esp_timer_get_time();
                    lastFrames = 0;
                    lastPackets = 0;
                    labStep = 0;
                    g_lab = LabState::Streaming;
                    ESP_LOGW(kTag, "LAB run %d: variant %d, arming %s", g_labRun, g_variant + 1,
                             kLabSequences[g_labRun % std::size(kLabSequences)].name);
                }
            } else if (event.kind == probe::UsbEvent::Kind::Detached &&
                       event.device == g_stream.device && g_stream.device != nullptr) {
                ESP_LOGW(kTag, "---- TP-7 detached while streaming");
                stopMidiIn(g_stream.device, true);
                g_midi.claimed = false;
                stopStream(client.handle(), true);
                g_lab = LabState::Idle;
            }
            continue;
        }
        if (g_stream.device == nullptr || g_lab != LabState::Streaming) continue;
        const std::int64_t now = esp_timer_get_time();
        if (now - lastTime >= 1'000'000) report(lastFrames, lastPackets, lastTime);
        const std::int64_t elapsed = now - attachedAt;
        const LabSequence& sequence = kLabSequences[g_labRun % std::size(kLabSequences)];
        while (labStep < sequence.count && elapsed >= std::int64_t{sequence.steps[labStep].ms} * 1000) {
            logMidiIn("before");
            sendMidiPacket(g_stream.device, sequence.steps[labStep].packet);
            ++labStep;
        }
        if (labStep == sequence.count &&
            elapsed >= std::int64_t{kLabMtpMs} * 1000) {
            usb_device_handle_t device = g_stream.device;
            logMidiIn("cycle end");
            stopMidiIn(device, false);
            stopStream(client.handle(), false, false);
            requestMtp(device);
            if (g_midi.claimed) usb_host_interface_release(client.handle(), device, g_midi.interface);
            g_midi.claimed = false;
            usb_host_device_close(client.handle(), device);
            g_stream.device = nullptr;
            g_lab = LabState::AwaitingMtp;
        }
    }
}
