///
/// TP-7 USB bring-up probe.
///
/// Prints everything needed to tell *why* a TP-7 does or does not work on an
/// ESP32-P4 host port, in the order things go wrong in practice:
///
///   1. Does anything attach at all?        (VBUS, port choice, cable)
///   2. Does it enumerate, and at what speed?
///   3. What does the TP-7 expose?          (UAC version, MIDI, MTP interfaces)
///   4. Can we switch it to MTP?            (SysEx over USB-MIDI)
///   5. Does an MTP session open, can we list objects, how fast is a read?
///
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "PtpSession.hpp"
#include "Tp7Midi.hpp"
#include "UsbClient.hpp"
#include "driver/gpio.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"

namespace {

constexpr const char* kTag = "tp7-probe";
constexpr std::uint16_t kTeenageEngineering = 0x2367;
constexpr std::uint8_t kClassAudio = 0x01;
constexpr std::uint8_t kSubclassAudioControl = 0x01;
constexpr std::uint8_t kSubclassMidiStreaming = 0x03;
constexpr std::uint8_t kClassStillImage = 0x06;
constexpr std::uint8_t kClassVendor = 0xFF;
constexpr std::uint8_t kAudioProtocolUac2 = 0x20;
constexpr std::uint8_t kSubclassAudioStreaming = 0x02;
constexpr std::uint8_t kDescriptorCsInterface = 0x24;
constexpr std::uint8_t kAsGeneral = 0x01;
constexpr std::uint8_t kAsFormatType = 0x02;

struct Endpoint {
    std::uint8_t address{0};
    std::uint16_t maxPacket{0};
};

struct MidiOut {
    bool found{false};
    std::uint8_t interface{0};
    std::uint8_t alternate{0};
    Endpoint out;
};

struct MtpPipes {
    bool found{false};
    std::uint8_t interface{0};
    std::uint8_t alternate{0};
    Endpoint bulkIn;
    Endpoint bulkOut;
};

struct Layout {
    MidiOut midi;
    MtpPipes mtp;
    int audioClass{0};  ///< 0 none, 1 UAC1, 2 UAC2.
    int isochronousEndpoints{0};
};

bool g_requestedMtp = false;

std::string toAscii(const usb_str_desc_t* descriptor) {
    std::string text;
    if (descriptor == nullptr) return "(none)";
    const int units = (descriptor->bLength - 2) / 2;
    for (int i = 0; i < units; ++i) {
        const std::uint16_t unit = descriptor->wData[i];
        text.push_back(unit < 0x80 ? static_cast<char>(unit) : '?');
    }
    return text;
}

const char* speedName(usb_speed_t speed) {
    switch (speed) {
        case USB_SPEED_LOW: return "low (1.5 Mbit/s)";
        case USB_SPEED_FULL: return "FULL (12 Mbit/s)";
        case USB_SPEED_HIGH: return "HIGH (480 Mbit/s)";
        default: return "unknown";
    }
}

Layout classify(const usb_config_desc_t* config) {
    Layout layout;
    int offset = 0;
    const auto* descriptor = reinterpret_cast<const usb_standard_desc_t*>(config);
    const usb_intf_desc_t* current = nullptr;
    Endpoint in;
    Endpoint out;
    bool hasInterruptIn = false;

    auto finishInterface = [&] {
        if (current == nullptr) return;
        const bool mtpShaped = in.address != 0 && out.address != 0 && hasInterruptIn;
        if (!layout.mtp.found && mtpShaped &&
            (current->bInterfaceClass == kClassStillImage ||
             current->bInterfaceClass == kClassVendor)) {
            layout.mtp = {true, current->bInterfaceNumber, current->bAlternateSetting, in, out};
        }
        if (!layout.midi.found && current->bInterfaceClass == kClassAudio &&
            current->bInterfaceSubClass == kSubclassMidiStreaming && out.address != 0) {
            layout.midi = {true, current->bInterfaceNumber, current->bAlternateSetting, out};
        }
    };

    while ((descriptor = usb_parse_next_descriptor(descriptor, config->wTotalLength, &offset))) {
        if (descriptor->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            finishInterface();
            current = reinterpret_cast<const usb_intf_desc_t*>(descriptor);
            in = {};
            out = {};
            hasInterruptIn = false;
            if (current->bInterfaceClass == kClassAudio &&
                current->bInterfaceSubClass == kSubclassAudioControl) {
                layout.audioClass = current->bInterfaceProtocol == kAudioProtocolUac2 ? 2 : 1;
            }
        } else if (descriptor->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT &&
                   current != nullptr) {
            const auto* ep = reinterpret_cast<const usb_ep_desc_t*>(descriptor);
            const int type = ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK;
            const bool isIn = (ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) != 0;
            const Endpoint endpoint{ep->bEndpointAddress,
                                    static_cast<std::uint16_t>(ep->wMaxPacketSize & 0x7FF)};
            if (type == USB_BM_ATTRIBUTES_XFER_BULK) (isIn ? in : out) = endpoint;
            if (type == USB_BM_ATTRIBUTES_XFER_INT && isIn) hasInterruptIn = true;
            if (type == USB_BM_ATTRIBUTES_XFER_ISOC) ++layout.isochronousEndpoints;
        }
    }
    finishInterface();
    return layout;
}

/// Prints one line per isochronous audio-streaming alternate setting: its
/// direction, channel count, sample format and how many frames one packet
/// carries. UAC1 lists its sample rates in the descriptor; UAC2 keeps them on
/// a clock entity, so for UAC2 the packet capacity is the hint.
void describeAudioStreaming(const usb_config_desc_t* config, int audioClass) {
    int offset = 0;
    const auto* descriptor = reinterpret_cast<const usb_standard_desc_t*>(config);
    const usb_intf_desc_t* current = nullptr;
    int channels = 0;
    int subslot = 0;
    int bits = 0;
    std::string rates;

    while ((descriptor = usb_parse_next_descriptor(descriptor, config->wTotalLength, &offset))) {
        const auto* raw = reinterpret_cast<const std::uint8_t*>(descriptor);
        if (descriptor->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            current = reinterpret_cast<const usb_intf_desc_t*>(descriptor);
            channels = subslot = bits = 0;
            rates.clear();
            continue;
        }
        if (current == nullptr || current->bInterfaceClass != kClassAudio ||
            current->bInterfaceSubClass != kSubclassAudioStreaming)
            continue;
        if (descriptor->bDescriptorType == kDescriptorCsInterface && raw[0] >= 6) {
            if (audioClass == 2 && raw[2] == kAsGeneral && raw[0] >= 11) {
                channels = raw[10];
            } else if (audioClass == 2 && raw[2] == kAsFormatType) {
                subslot = raw[4];
                bits = raw[5];
            } else if (audioClass == 1 && raw[2] == kAsFormatType && raw[0] >= 8) {
                channels = raw[4];
                subslot = raw[5];
                bits = raw[6];
                for (int i = 0; i < raw[7] && 8 + 3 * i + 2 < raw[0]; ++i) {
                    const unsigned hz = raw[8 + 3 * i] | (raw[9 + 3 * i] << 8) |
                                        (raw[10 + 3 * i] << 16);
                    rates += (rates.empty() ? "" : "/") + std::to_string(hz);
                }
            }
        } else if (descriptor->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const auto* ep = reinterpret_cast<const usb_ep_desc_t*>(descriptor);
            if ((ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) !=
                    USB_BM_ATTRIBUTES_XFER_ISOC ||
                (ep->bmAttributes & 0x30) == 0x10)  // Explicit feedback endpoint.
                continue;
            // High-speed high-bandwidth endpoints move up to three transactions
            // per microframe (bits 12:11). The ESP-IDF v5.5 host controller
            // driver ignores those bits and schedules one.
            const int transactions = 1 + ((ep->wMaxPacketSize >> 11) & 3);
            const int packet = (ep->wMaxPacketSize & 0x7FF) * transactions;
            const int intervalUs = 125 << (ep->bInterval - 1);  // High speed.
            const int frameBytes = channels * subslot;
            const int frames = frameBytes > 0 ? packet / frameBytes : 0;
            ESP_LOGI(kTag,
                     "    iface %u alt %u: %s ep 0x%02x, %d ch, %d-bit in %d bytes, %d bytes "
                     "per %d us = %d frames (~%d kHz max)%s%s%s",
                     current->bInterfaceNumber, current->bAlternateSetting,
                     (ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) ? "IN " : "OUT",
                     ep->bEndpointAddress, channels, bits, subslot, packet, intervalUs, frames,
                     intervalUs > 0 ? frames * 1000 / intervalUs : 0,
                     rates.empty() ? "" : ", rates ", rates.c_str(),
                     transactions > 1 ? ", HIGH-BANDWIDTH (several transactions/uframe)" : "");
        }
    }
}

/// The host library always activates configuration 1. Composite devices such
/// as the TP-7 offer others (Linux may pick a different one), so read and
/// summarise every configuration the device offers.
void surveyConfigurations(usb_host_client_handle_t client, usb_device_handle_t device,
                          std::uint8_t count) {
    for (std::uint8_t index = 0; index < count; ++index) {
        std::vector<std::uint8_t> bytes;
        if (const esp_err_t err = probe::readConfigDescriptor(client, device, index, bytes);
            err != ESP_OK) {
            ESP_LOGE(kTag, "configuration index %u: GET_DESCRIPTOR failed (%s)", index,
                     esp_err_to_name(err));
            continue;
        }
        const auto* config = reinterpret_cast<const usb_config_desc_t*>(bytes.data());
        if (bytes.size() < sizeof(usb_config_desc_t) || config->wTotalLength > bytes.size()) {
            ESP_LOGE(kTag, "configuration index %u: truncated (%u bytes)", index,
                     static_cast<unsigned>(bytes.size()));
            continue;
        }
        const Layout layout = classify(config);
        ESP_LOGI(kTag, "configuration %u of %u (bConfigurationValue %u, %u bytes): audio=%s, "
                       "isochronous endpoints=%d, MIDI OUT=%s, MTP=%s",
                 index + 1, count, config->bConfigurationValue, config->wTotalLength,
                 layout.audioClass == 2 ? "UAC2" : layout.audioClass == 1 ? "UAC1" : "none",
                 layout.isochronousEndpoints, layout.midi.found ? "yes" : "no",
                 layout.mtp.found ? "yes" : "no");
        describeAudioStreaming(config, layout.audioClass);
    }
}

struct Throughput {
    std::uint64_t bytes{0};
};

bool countBytes(std::span<const std::uint8_t> chunk, void* context) {
    static_cast<Throughput*>(context)->bytes += chunk.size();
    return true;
}

void describeDevice(probe::PtpSession& session) {
    using namespace probe;
    std::vector<std::uint8_t> data;
    PtpResponse response;
    if (session.transact(ptp::kGetDeviceInfo, {}, data, response) != ESP_OK ||
        response.code != ptp::kResponseOk) {
        ESP_LOGE(kTag, "GetDeviceInfo failed (response 0x%04x)", response.code);
        return;
    }
    DatasetReader reader{data};
    reader.u16();                                  // StandardVersion
    reader.u32();                                  // VendorExtensionID
    reader.u16();                                  // VendorExtensionVersion
    const std::string extensions = reader.string();
    reader.u16();                                  // FunctionalMode
    const auto operations = reader.u16Array();
    reader.u16Array();                             // Events
    reader.u16Array();                             // Device properties
    reader.u16Array();                             // Capture formats
    reader.u16Array();                             // Playback formats
    const std::string manufacturer = reader.string();
    const std::string model = reader.string();
    const std::string version = reader.string();
    const std::string serial = reader.string();

    ESP_LOGI(kTag, "MTP device: %s %s, firmware %s, serial %s", manufacturer.c_str(),
             model.c_str(), version.c_str(), serial.c_str());
    ESP_LOGI(kTag, "MTP extensions: %s", extensions.c_str());
    auto supports = [&](std::uint16_t op) {
        for (const auto candidate : operations)
            if (candidate == op) return "yes";
        return "NO";
    };
    ESP_LOGI(kTag, "  GetPartialObject 0x101B: %s   GetPartialObject64 0x95C1: %s",
             supports(0x101B), supports(0x95C1));
    ESP_LOGI(kTag, "  SendObjectInfo 0x100C: %s   SendObject 0x100D: %s   DeleteObject 0x100B: %s",
             supports(0x100C), supports(0x100D), supports(0x100B));
    ESP_LOGI(kTag, "  GetObjectPropList 0x9805: %s", supports(0x9805));
}

struct ObjectSummary {
    std::uint32_t handle{0};
    std::string name;
    std::uint32_t size{0};
    bool folder{false};
};

bool objectInfo(probe::PtpSession& session, std::uint32_t handle, ObjectSummary& summary) {
    using namespace probe;
    std::vector<std::uint8_t> data;
    PtpResponse response;
    if (session.transact(ptp::kGetObjectInfo, {handle}, data, response) != ESP_OK ||
        response.code != ptp::kResponseOk)
        return false;
    DatasetReader reader{data};
    reader.u32();                                  // StorageID
    const std::uint16_t format = reader.u16();
    reader.u16();                                  // ProtectionStatus
    const std::uint32_t size = reader.u32();
    reader.skip(2 + 4 * 6);                        // Thumb/Image fields
    reader.u32();                                  // ParentObject
    reader.u16();                                  // AssociationType
    reader.u32();                                  // AssociationDesc
    reader.u32();                                  // SequenceNumber
    summary = {handle, reader.string(), size, format == ptp::kFormatAssociation};
    return reader.ok();
}

std::vector<ObjectSummary> listChildren(probe::PtpSession& session, std::uint32_t storage,
                                        std::uint32_t parent, const char* indent) {
    using namespace probe;
    std::vector<ObjectSummary> children;
    std::vector<std::uint8_t> data;
    PtpResponse response;
    if (session.transact(ptp::kGetObjectHandles, {storage, 0, parent}, data, response) !=
            ESP_OK ||
        response.code != ptp::kResponseOk) {
        ESP_LOGE(kTag, "GetObjectHandles failed (response 0x%04x)", response.code);
        return children;
    }
    DatasetReader reader{data};
    const auto handles = reader.u32Array();
    ESP_LOGI(kTag, "%s%u objects", indent, static_cast<unsigned>(handles.size()));
    for (const std::uint32_t handle : handles) {
        if (children.size() >= static_cast<std::size_t>(CONFIG_PROBE_LIST_OBJECTS)) break;
        ObjectSummary summary;
        if (!objectInfo(session, handle, summary)) continue;
        ESP_LOGI(kTag, "%s%s %-32s %10" PRIu32, indent, summary.folder ? "[dir]" : "     ",
                 summary.name.c_str(), summary.size);
        children.push_back(summary);
    }
    return children;
}

void measureThroughput(probe::PtpSession& session, const ObjectSummary& file) {
    using namespace probe;
    const std::uint32_t chunk = 1024 * 1024;
    const std::uint64_t limit =
        std::min<std::uint64_t>(file.size, std::uint64_t{CONFIG_PROBE_THROUGHPUT_MIB} * chunk);
    Throughput throughput;
    const std::int64_t start = esp_timer_get_time();
    for (std::uint64_t offset = 0; offset < limit; offset += chunk) {
        PtpResponse response;
        const auto wanted = static_cast<std::uint32_t>(std::min<std::uint64_t>(chunk, limit - offset));
        if (session.transact(ptp::kGetPartialObject,
                             {file.handle, static_cast<std::uint32_t>(offset), wanted}, &countBytes,
                             &throughput, response, 10000) != ESP_OK ||
            response.code != ptp::kResponseOk) {
            ESP_LOGE(kTag, "GetPartialObject failed at offset %" PRIu64 " (response 0x%04x)",
                     offset, response.code);
            return;
        }
    }
    const double seconds = static_cast<double>(esp_timer_get_time() - start) / 1e6;
    const double mib = static_cast<double>(throughput.bytes) / (1024.0 * 1024.0);
    ESP_LOGI(kTag, "Read %.1f MiB of '%s' in %.2f s = %.2f MiB/s", mib, file.name.c_str(),
             seconds, mib / seconds);
    ESP_LOGI(kTag, "(a 12-channel 24-bit 48 kHz TP-7 take is 1.65 MiB per second of audio)");
}

void runMtp(usb_host_client_handle_t client, usb_device_handle_t device, const MtpPipes& mtp) {
    using namespace probe;
    if (usb_host_interface_claim(client, device, mtp.interface, mtp.alternate) != ESP_OK) {
        ESP_LOGE(kTag, "could not claim MTP interface %u", mtp.interface);
        return;
    }
    {
        BulkPipe out{device, mtp.bulkOut.address, mtp.bulkOut.maxPacket};
        BulkPipe in{device, mtp.bulkIn.address, mtp.bulkIn.maxPacket};
        if (out.init(512) != ESP_OK || in.init(64 * 1024) != ESP_OK) {
            ESP_LOGE(kTag, "could not allocate MTP transfers");
        } else {
            PtpSession session{out, in};
            describeDevice(session);
            if (session.open() == ESP_OK) {
                ESP_LOGI(kTag, "MTP session open");
                std::vector<std::uint8_t> data;
                PtpResponse response;
                std::vector<std::uint32_t> storages;
                if (session.transact(ptp::kGetStorageIds, {}, data, response) == ESP_OK &&
                    response.code == ptp::kResponseOk) {
                    DatasetReader reader{data};
                    storages = reader.u32Array();
                }
                for (const std::uint32_t storage : storages) {
                    if (session.transact(ptp::kGetStorageInfo, {storage}, data, response) ==
                            ESP_OK &&
                        response.code == ptp::kResponseOk) {
                        DatasetReader reader{data};
                        reader.u16();
                        reader.u16();
                        reader.u16();
                        const std::uint64_t capacity = reader.u64();
                        const std::uint64_t free = reader.u64();
                        reader.u32();
                        const std::string description = reader.string();
                        ESP_LOGI(kTag, "storage 0x%08" PRIx32 " '%s': %" PRIu64 " of %" PRIu64
                                       " MiB free",
                                 storage, description.c_str(), free >> 20, capacity >> 20);
                    }
                    ObjectSummary largest;
                    for (const auto& entry : listChildren(session, storage, ptp::kRootParent, "  ")) {
                        if (entry.folder && entry.name == "recordings") {
                            for (const auto& take : listChildren(session, storage, entry.handle, "    "))
                                if (!take.folder && take.size > largest.size) largest = take;
                        }
                        if (!entry.folder && entry.size > largest.size) largest = entry;
                    }
                    if (CONFIG_PROBE_THROUGHPUT_MIB > 0 && largest.size > 0)
                        measureThroughput(session, largest);
                }
                ESP_LOGI(kTag, "closing MTP session; the TP-7 should return to USB audio");
                (void)session.close();
            }
        }
    }
    usb_host_interface_release(client, device, mtp.interface);
}

void requestMtp(usb_host_client_handle_t client, usb_device_handle_t device, const MidiOut& midi) {
    using namespace probe;
    if (usb_host_interface_claim(client, device, midi.interface, midi.alternate) != ESP_OK) {
        ESP_LOGE(kTag, "could not claim MIDI interface %u", midi.interface);
        return;
    }
    {
        BulkPipe out{device, midi.out.address, midi.out.maxPacket};
        if (out.init(64) == ESP_OK && sendTp7EnterMtp(out) == ESP_OK) {
            g_requestedMtp = true;
            ESP_LOGI(kTag, "MTP request sent; expect a detach, then an MTP device to attach");
        } else {
            ESP_LOGE(kTag, "sending the MTP SysEx failed (%s)",
                     transferStatusName(out.lastStatus()));
        }
    }
    usb_host_interface_release(client, device, midi.interface);
}

void probeDevice(usb_host_client_handle_t client, std::uint8_t address) {
    usb_device_handle_t device = nullptr;
    if (const esp_err_t err = usb_host_device_open(client, address, &device); err != ESP_OK) {
        ESP_LOGE(kTag, "open device %u failed: %s", address, esp_err_to_name(err));
        return;
    }
    usb_device_info_t info{};
    const usb_device_desc_t* deviceDescriptor = nullptr;
    const usb_config_desc_t* config = nullptr;
    usb_host_device_info(device, &info);
    usb_host_get_device_descriptor(device, &deviceDescriptor);
    usb_host_get_active_config_descriptor(device, &config);

    ESP_LOGI(kTag, "---- device %u attached: %04x:%04x '%s' '%s', speed %s", address,
             deviceDescriptor->idVendor, deviceDescriptor->idProduct,
             toAscii(info.str_desc_manufacturer).c_str(), toAscii(info.str_desc_product).c_str(),
             speedName(info.speed));
    usb_print_device_descriptor(deviceDescriptor);
    usb_print_config_descriptor(config, nullptr);

    const Layout layout = classify(config);
    ESP_LOGI(kTag,
             "summary (active configuration %u): audio=%s, isochronous endpoints=%d, "
             "MIDI OUT=%s, MTP=%s",
             config->bConfigurationValue, layout.audioClass == 2 ? "UAC2" : layout.audioClass == 1 ? "UAC1" : "none",
             layout.isochronousEndpoints, layout.midi.found ? "yes" : "no",
             layout.mtp.found ? "yes" : "no");
    if (deviceDescriptor->bNumConfigurations > 1)
        surveyConfigurations(client, device, deviceDescriptor->bNumConfigurations);
    if (info.speed != USB_SPEED_HIGH)
        ESP_LOGW(kTag, "not high speed: MTP will be slow and TP-7 monitor audio cannot fit");

    const bool isTp7 = deviceDescriptor->idVendor == kTeenageEngineering;
    if (layout.mtp.found) {
        runMtp(client, device, layout.mtp);
    } else if (isTp7 && layout.midi.found && CONFIG_PROBE_REQUEST_MTP && !g_requestedMtp) {
        requestMtp(client, device, layout.midi);
    }
    usb_host_device_close(client, device);
}

void hostLibraryTask(void* notify) {
    usb_host_config_t config{};
    config.intr_flags = ESP_INTR_FLAG_LEVEL1;
#if CONFIG_PROBE_USB_PORT_FS
    config.peripheral_map = BIT1;
#else
    config.peripheral_map = BIT0;
#endif
    ESP_ERROR_CHECK(usb_host_install(&config));
    ESP_LOGI(kTag, "USB host installed on the %s port",
             config.peripheral_map == BIT0 ? "high-speed" : "full-speed");
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
    gpio_set_level(pin, CONFIG_PROBE_VBUS_ENABLE_ACTIVE_HIGH ? 1 : 0);
    ESP_LOGI(kTag, "VBUS enabled on GPIO%d", CONFIG_PROBE_VBUS_ENABLE_GPIO);
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
    ESP_LOGI(kTag, "waiting for a USB device; plug in the TP-7");

    for (;;) {
        probe::UsbEvent event;
        if (!client.waitEvent(event, pdMS_TO_TICKS(10000))) {
            usb_host_lib_info_t info{};
            usb_host_lib_info(&info);
            if (info.num_devices == 0)
                ESP_LOGW(kTag, "still no device. Check VBUS (5 V on the socket), the port and "
                               "the cable - see README");
            continue;
        }
        if (event.kind == probe::UsbEvent::Kind::Attached) {
            probeDevice(client.handle(), event.address);
        } else {
            ESP_LOGI(kTag, "---- device detached");
        }
    }
}
