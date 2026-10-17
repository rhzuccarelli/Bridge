#include "aw/platform/esp32p4/EspUsb.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "aw/core/Logger.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/BulkPipe.hpp"
#include "usb/EspUsbStack.hpp"
#include "usb/PtpSession.hpp"
#include "usb/Tp7AudioOutput.hpp"
#include "usb/Tp7MidiPort.hpp"

namespace aw::platform::esp32p4 {
namespace {

using usb::BulkPipe;
using usb::EspUsbStack;
using usb::Lease;
using usb::PtpDataSink;
using usb::PtpResponse;
using usb::PtpSession;
namespace ptp = usb::ptp;

constexpr std::string_view kModule = "usb";
constexpr std::uint16_t kTeenageEngineering = 0x2367;
constexpr std::size_t kPipeBytes = 64 * 1024;
constexpr std::uint32_t kReadChunk = 1024 * 1024;

std::string hex4(std::uint16_t value) {
    char text[8];
    std::snprintf(text, sizeof(text), "0x%04x", value);
    return text;
}

// --- USB host ---------------------------------------------------------------

class EspUsbHost final : public hal::USBInterface {
public:
    explicit EspUsbHost(std::shared_ptr<EspUsbStack> stack) : stack_{std::move(stack)} {}

    core::Status start() override { return stack_->start(); }
    void stop() noexcept override { stack_->setListener(nullptr); }
    void setHotplugListener(hal::UsbHotplugListener* listener) noexcept override {
        stack_->setListener(listener);
    }
    core::Result<std::vector<hal::UsbDeviceInfo>> enumerate() const override {
        return stack_->devices();
    }

private:
    std::shared_ptr<EspUsbStack> stack_;
};

// --- TP-7 mode switch ---------------------------------------------------------

/// Captured from Teenage Engineering Field Kit 2.0.1 switching a TP-7 on OS
/// 1.1.11 from its USB audio/MIDI personality into MTP; the same bytes
/// `AlsaTp7ModeSwitcher` sends on the Pi.
constexpr std::array<std::uint8_t, 13> kEnterMtp{
    0xF0, 0x00, 0x20, 0x76, 0x19, 0x40, 0x60, 0x3C, 0x04, 0x00, 0x01, 0x03, 0xF7,
};

/// USB-MIDI 1.0 section 4: SysEx travels in 4-byte event packets on cable 0.
/// CIN 0x4 carries three bytes of a continuing message; CIN 0x5/0x6/0x7 end it
/// with one, two or three bytes.
std::vector<std::uint8_t> packSysEx(std::span<const std::uint8_t> bytes) {
    std::vector<std::uint8_t> packets;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t left = bytes.size() - offset;
        const std::size_t take = left > 3 ? 3 : left;
        const bool last = take == left;
        packets.push_back(!last ? 0x4 : static_cast<std::uint8_t>(0x4 + take));
        for (std::size_t i = 0; i < 3; ++i) packets.push_back(i < take ? bytes[offset + i] : 0);
        offset += take;
    }
    return packets;
}

class EspTp7ModeSwitcher final : public hal::DeviceModeInterface {
public:
    EspTp7ModeSwitcher(std::shared_ptr<EspUsbStack> stack, std::shared_ptr<usb::Tp7MidiPort> midi)
        : stack_{std::move(stack)}, midi_{std::move(midi)} {}

    core::Status requestMtpMode() override {
        for (const hal::UsbDeviceInfo& device : stack_->devices()) {
            if (device.vendorId == kTeenageEngineering && device.supportsMtp) {
                return core::success();  // Already in MTP mode.
            }
        }
        const core::Status result = midi_->send(packSysEx(kEnterMtp));
        if (result) core::log::info(kModule, "asked the TP-7 to switch to MTP");
        return result;
    }

private:
    std::shared_ptr<EspUsbStack> stack_;
    std::shared_ptr<usb::Tp7MidiPort> midi_;
};

// --- MTP --------------------------------------------------------------------

/// "YYYYMMDDThhmmss" (any suffix ignored), read as UTC. 0 when unparseable.
std::int64_t parsePtpDate(const std::string& text) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (std::sscanf(text.c_str(), "%4d%2d%2dT%2d%2d%2d", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
    // Days from civil (Howard Hinnant), valid for the proleptic Gregorian calendar.
    y -= mo <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const std::int64_t days = static_cast<std::int64_t>(era) * 146097 + doe - 719468;
    return days * 86400 + h * 3600 + mi * 60 + s;
}

core::Error responseError(std::uint16_t operation, std::uint16_t code) {
    const std::string what = "MTP " + hex4(operation) + " answered " + hex4(code);
    switch (code) {
        case ptp::kInvalidObjectHandle: return core::makeError(core::ErrorCode::NotFound, what);
        case ptp::kStoreFull: return core::makeError(core::ErrorCode::OutOfSpace, what);
        case ptp::kDeviceBusy: return core::makeError(core::ErrorCode::DeviceBusy, what);
        default: return core::makeError(core::ErrorCode::ProtocolError, what);
    }
}

core::Error transferError(std::uint16_t operation, esp_err_t err) {
    const std::string what = "MTP " + hex4(operation) + ": " + esp_err_to_name(err);
    if (err == ESP_ERR_INVALID_STATE) return core::makeError(core::ErrorCode::DeviceNotConnected, what);
    if (err == ESP_ERR_TIMEOUT) return core::makeError(core::ErrorCode::Timeout, what);
    return core::makeError(core::ErrorCode::IoError, what);
}

class EspMtpDevice final : public hal::MTPInterface {
public:
    explicit EspMtpDevice(std::shared_ptr<EspUsbStack> stack) : stack_{std::move(stack)} {}
    ~EspMtpDevice() override { closeSession(); }

    core::Status openSession(std::uint32_t deviceKey) override {
        std::lock_guard lock{mutex_};
        closeLocked();
        const auto lease = stack_->acquire([deviceKey](const hal::UsbDeviceInfo& info, const usb::Layout& layout) {
            return info.key() == deviceKey && layout.hasMtp;
        });
        if (!lease) return core::makeError(core::ErrorCode::DeviceNotConnected, "MTP device gone");
        const usb::Layout& layout = lease->layout;
        if (usb_host_interface_claim(stack_->client(), lease->handle, layout.mtpInterface,
                                     layout.mtpAlternate) != ESP_OK) {
            stack_->release(*lease);
            return core::makeError(core::ErrorCode::DeviceBusy, "cannot claim MTP interface");
        }
        auto open = std::make_unique<Open>(*lease);
        if (open->out.init(kPipeBytes) != ESP_OK || open->in.init(kPipeBytes) != ESP_OK) {
            open_ = std::move(open);
            closeLocked();
            return core::makeError(core::ErrorCode::OutOfMemory, "MTP transfer buffers");
        }
        const esp_err_t err = open->session.open();
        open->opened = err == ESP_OK;
        open_ = std::move(open);
        if (err != ESP_OK) {
            closeLocked();
            return core::makeError(core::ErrorCode::ProtocolError,
                                   std::string{"OpenSession failed: "} + esp_err_to_name(err));
        }
        hasSession_.store(true);
        return core::success();
    }

    void closeSession() noexcept override {
        std::lock_guard lock{mutex_};
        closeLocked();
    }

    bool hasSession() const noexcept override { return hasSession_.load(); }

    core::Result<hal::MtpDeviceInfo> deviceInfo() const override {
        std::lock_guard lock{mutex_};
        std::vector<std::uint8_t> data;
        if (auto status = run(ptp::kGetDeviceInfo, {}, data); !status) return status.error();
        usb::DatasetReader reader{data};
        reader.u16();          // StandardVersion
        reader.u32();          // VendorExtensionID
        reader.u16();          // VendorExtensionVersion
        reader.string();       // VendorExtensionDesc
        reader.u16();          // FunctionalMode
        reader.u16Array();     // Operations
        reader.u16Array();     // Events
        reader.u16Array();     // Device properties
        reader.u16Array();     // Capture formats
        reader.u16Array();     // Playback formats
        hal::MtpDeviceInfo info;
        info.manufacturer = reader.string();
        info.model = reader.string();
        info.firmwareVersion = reader.string();
        info.serialNumber = reader.string();
        if (!reader.ok()) return core::makeError(core::ErrorCode::Corrupt, "short DeviceInfo");
        return info;
    }

    core::Result<std::vector<hal::MtpStorageInfo>> storages() const override {
        std::lock_guard lock{mutex_};
        std::vector<std::uint8_t> data;
        if (auto status = run(ptp::kGetStorageIds, {}, data); !status) return status.error();
        const auto ids = usb::DatasetReader{data}.u32Array();
        std::vector<hal::MtpStorageInfo> result;
        for (const std::uint32_t id : ids) {
            if (auto status = run(ptp::kGetStorageInfo, {id}, data); !status) return status.error();
            usb::DatasetReader reader{data};
            reader.u16();  // StorageType
            reader.u16();  // FilesystemType
            reader.u16();  // AccessCapability
            hal::MtpStorageInfo info;
            info.id = id;
            info.capacityBytes = reader.u64();
            info.freeBytes = reader.u64();
            reader.u32();  // FreeSpaceInObjects
            info.description = reader.string();
            result.push_back(std::move(info));
        }
        return result;
    }

    core::Result<std::vector<hal::MtpObjectInfo>> listObjects(hal::MtpStorageId storage,
                                                              hal::MtpObjectHandle parent) override {
        std::lock_guard lock{mutex_};
        return listLocked(storage, parent);
    }

    core::Result<hal::MtpObjectInfo> objectInfo(hal::MtpObjectHandle handle) override {
        std::lock_guard lock{mutex_};
        return objectInfoLocked(handle);
    }

    core::Status readObject(hal::MtpObjectHandle handle, const hal::MtpWriteSink& sink,
                            const hal::MtpProgressCallback& progress) override {
        std::lock_guard lock{mutex_};
        auto info = objectInfoLocked(handle);
        if (!info) return info.error();
        const std::uint64_t total = info.value().sizeBytes;
        if (total >= 0xFFFFFFFFu) {
            // GetPartialObject takes 32-bit offsets; the TP-7 lacks the 64-bit variant.
            return core::makeError(core::ErrorCode::UnsupportedFormat, "object over 4 GiB");
        }
        std::uint64_t done = 0;
        bool cancelled = false;
        while (done < total && !cancelled) {
            const auto wanted =
                static_cast<std::uint32_t>(std::min<std::uint64_t>(kReadChunk, total - done));
            const PtpDataSink collect = [&](std::span<const std::uint8_t> chunk) {
                if (!sink(std::as_bytes(chunk))) cancelled = true;
                return !cancelled;
            };
            if (auto status = run(ptp::kGetPartialObject,
                                  {handle, static_cast<std::uint32_t>(done), wanted}, collect, 10000);
                !status)
                return status;
            done += wanted;
            if (progress && !progress(done, total)) cancelled = true;
        }
        if (cancelled) return core::makeError(core::ErrorCode::Cancelled, "MTP read cancelled");
        return core::success();
    }

    core::Result<hal::MtpObjectHandle> writeObject(hal::MtpStorageId storage,
                                                   hal::MtpObjectHandle parent,
                                                   const std::string& name, std::uint64_t sizeBytes,
                                                   const hal::MtpReadSource& source,
                                                   const hal::MtpProgressCallback& progress) override {
        std::lock_guard lock{mutex_};
        if (sizeBytes >= 0xFFFFFFFFu) {
            return core::makeError(core::ErrorCode::UnsupportedFormat, "object over 4 GiB");
        }
        auto siblings = listLocked(storage, parent);
        if (!siblings) return siblings.error();
        for (const auto& sibling : siblings.value()) {
            if (sibling.name == name) {
                return core::makeError(core::ErrorCode::AlreadyExists, name + " exists on device");
            }
        }

        const bool wav = name.size() > 4 && (name.ends_with(".wav") || name.ends_with(".WAV"));
        usb::DatasetWriter dataset;
        dataset.u32(storage);
        dataset.u16(wav ? ptp::kFormatWav : ptp::kFormatUndefined);
        dataset.u16(0);                                   // ProtectionStatus
        dataset.u32(static_cast<std::uint32_t>(sizeBytes));
        dataset.u16(0);                                   // ThumbFormat
        for (int i = 0; i < 6; ++i) dataset.u32(0);       // Thumb and image fields
        dataset.u32(parent);
        dataset.u16(0);                                   // AssociationType
        dataset.u32(0);                                   // AssociationDesc
        dataset.u32(0);                                   // SequenceNumber
        dataset.string(name);
        dataset.string("");                               // DateCreated
        dataset.string("");                               // DateModified
        dataset.string("");                               // Keywords

        const auto& bytes = dataset.bytes();
        std::size_t sent = 0;
        PtpResponse response;
        if (auto status = runOut(ptp::kSendObjectInfo, {storage, parent}, bytes.size(),
                                 [&](std::span<std::uint8_t> buffer) {
                                     const std::size_t n = std::min(buffer.size(), bytes.size() - sent);
                                     std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(sent), n,
                                                 buffer.begin());
                                     sent += n;
                                     return n;
                                 },
                                 response);
            !status)
            return status.error();
        if (response.paramCount < 3) {
            return core::makeError(core::ErrorCode::ProtocolError, "SendObjectInfo gave no handle");
        }
        const hal::MtpObjectHandle handle = response.params[2];

        // A PTP data phase cannot be aborted, so a cancel during it is honoured
        // afterwards by deleting the half-wanted object.
        std::uint64_t done = 0;
        bool cancelled = false;
        if (auto status = runOut(ptp::kSendObject, {}, sizeBytes,
                                 [&](std::span<std::uint8_t> buffer) {
                                     const std::size_t n = source(std::as_writable_bytes(buffer));
                                     done += n;
                                     if (progress && !cancelled && !progress(done, sizeBytes))
                                         cancelled = true;
                                     return n;
                                 },
                                 response);
            !status)
            return status.error();
        if (cancelled) {
            (void)run(ptp::kDeleteObject, {handle, 0}, PtpDataSink{});
            return core::makeError(core::ErrorCode::Cancelled, "MTP write cancelled");
        }
        return handle;
    }

    core::Status deleteObject(hal::MtpObjectHandle handle) override {
        std::lock_guard lock{mutex_};
        return run(ptp::kDeleteObject, {handle, 0}, PtpDataSink{});
    }

private:
    struct Open {
        explicit Open(Lease held)
            : lease{std::move(held)},
              out{lease.handle, lease.layout.bulkOut.address, lease.layout.bulkOut.maxPacket},
              in{lease.handle, lease.layout.bulkIn.address, lease.layout.bulkIn.maxPacket},
              session{out, in} {}
        Lease lease;
        BulkPipe out;
        BulkPipe in;
        PtpSession session;
        bool opened{false};  ///< OpenSession succeeded, so CloseSession is owed.
    };

    void closeLocked() noexcept {
        if (!open_) return;
        hasSession_.store(false);
        // Closing the session is what returns the TP-7 to USB audio. After an
        // unplug there is nobody to tell.
        if (open_->opened && !stack_->isGone(open_->lease)) (void)open_->session.close();
        const Lease lease = open_->lease;
        open_.reset();  // Frees the transfers before the interface goes.
        (void)usb_host_interface_release(stack_->client(), lease.handle, lease.layout.mtpInterface);
        stack_->release(lease);
    }

    core::Status checked(std::uint16_t operation, esp_err_t err, const PtpResponse& response) const {
        if (err != ESP_OK) return transferError(operation, err);
        if (response.code != ptp::kResponseOk) return responseError(operation, response.code);
        return core::success();
    }

    core::Status run(std::uint16_t operation, std::initializer_list<std::uint32_t> params,
                     const PtpDataSink& sink, std::uint32_t timeoutMs = 5000) const {
        if (!open_) return core::makeError(core::ErrorCode::InvalidState, "no MTP session");
        PtpResponse response;
        return checked(operation, open_->session.transact(operation, params, sink, response, timeoutMs),
                       response);
    }

    core::Status run(std::uint16_t operation, std::initializer_list<std::uint32_t> params,
                     std::vector<std::uint8_t>& data) const {
        if (!open_) return core::makeError(core::ErrorCode::InvalidState, "no MTP session");
        PtpResponse response;
        return checked(operation, open_->session.transact(operation, params, data, response),
                       response);
    }

    core::Status runOut(std::uint16_t operation, std::initializer_list<std::uint32_t> params,
                        std::uint64_t bytes, const usb::PtpDataSource& source,
                        PtpResponse& response) {
        if (!open_) return core::makeError(core::ErrorCode::InvalidState, "no MTP session");
        return checked(operation,
                       open_->session.transactOut(operation, params, bytes, source, response),
                       response);
    }

    core::Result<hal::MtpObjectInfo> objectInfoLocked(hal::MtpObjectHandle handle) const {
        std::vector<std::uint8_t> data;
        if (auto status = run(ptp::kGetObjectInfo, {handle}, data); !status) return status.error();
        usb::DatasetReader reader{data};
        hal::MtpObjectInfo info;
        info.handle = handle;
        info.storageId = reader.u32();
        const std::uint16_t format = reader.u16();
        reader.u16();                   // ProtectionStatus
        info.sizeBytes = reader.u32();  // 0xFFFFFFFF means 4 GiB or more.
        reader.skip(2 + 4 * 6);         // Thumb and image fields
        info.parent = reader.u32();
        reader.u16();                   // AssociationType
        reader.u32();                   // AssociationDesc
        reader.u32();                   // SequenceNumber
        info.name = reader.string();
        reader.string();                // DateCreated
        info.modifiedUnixSeconds = parsePtpDate(reader.string());
        info.isFolder = format == ptp::kFormatAssociation;
        if (info.parent == 0) info.parent = hal::kMtpRootHandle;
        if (info.name.empty()) return core::makeError(core::ErrorCode::Corrupt, "short ObjectInfo");
        return info;
    }

    core::Result<std::vector<hal::MtpObjectInfo>> listLocked(hal::MtpStorageId storage,
                                                             hal::MtpObjectHandle parent) const {
        std::vector<std::uint8_t> data;
        if (auto status = run(ptp::kGetObjectHandles, {storage, 0, parent}, data); !status)
            return status.error();
        std::vector<hal::MtpObjectInfo> objects;
        for (const std::uint32_t handle : usb::DatasetReader{data}.u32Array()) {
            auto info = objectInfoLocked(handle);
            if (!info) return info.error();
            objects.push_back(std::move(info).value());
        }
        return objects;
    }

    std::shared_ptr<EspUsbStack> stack_;
    mutable std::mutex mutex_;
    std::unique_ptr<Open> open_;
    std::atomic<bool> hasSession_{false};
};

// --- TP-7 reset -------------------------------------------------------------

/// Makes a TP-7 in USB-audio mode re-enumerate, on a task of its own: the MTP
/// SysEx, then a PTP session opened and closed, which sends it back to audio.
/// `Tp7AudioOutput` asks for this when the TP-7's feedback is stuck.
void resetTp7(std::shared_ptr<EspUsbStack> stack, std::shared_ptr<usb::Tp7MidiPort> midi,
              hal::MTPInterface& mtp) {
    static std::atomic<bool> busy{false};
    if (busy.exchange(true)) return;
    struct Job {
        std::shared_ptr<EspUsbStack> stack;
        std::shared_ptr<usb::Tp7MidiPort> midi;
        hal::MTPInterface* mtp;
    };
    auto* job = new Job{std::move(stack), std::move(midi), &mtp};
    const auto run = [](void* context) {
        std::unique_ptr<Job> job{static_cast<Job*>(context)};
        if (auto status = job->midi->send(packSysEx(kEnterMtp)); !status) {
            core::log::warning(kModule, "TP-7 reset: " + status.error().describe());
        } else {
            std::optional<hal::UsbDeviceInfo> device;
            for (int i = 0; i < 80 && !device; ++i) {
                vTaskDelay(pdMS_TO_TICKS(100));
                for (const hal::UsbDeviceInfo& info : job->stack->devices())
                    if (info.vendorId == kTeenageEngineering && info.supportsMtp) device = info;
            }
            if (!device) {
                core::log::warning(kModule, "TP-7 reset: it did not come back in MTP mode");
            } else if (job->mtp->hasSession()) {
                core::log::info(kModule, "TP-7 reset: an MTP session is open; leaving it in MTP mode");
            } else if (auto status = job->mtp->openSession(device->key()); status) {
                job->mtp->closeSession();  // Closing the session returns it to USB audio.
                core::log::info(kModule, "TP-7 reset: back to USB audio");
            } else {
                core::log::warning(kModule, "TP-7 reset: " + status.error().describe());
            }
        }
        busy.store(false);
        vTaskDelete(nullptr);
    };
    if (xTaskCreate(run, "tp7_reset", 6144, job, 4, nullptr) != pdPASS) {
        delete job;
        busy.store(false);
    }
}

}  // namespace

EspUsbBackends createUsbBackends() {
    auto stack = std::make_shared<EspUsbStack>();
    EspUsbBackends backends;
    backends.usb = std::make_unique<EspUsbHost>(stack);
    backends.mtp = std::make_unique<EspMtpDevice>(stack);
    auto midi = std::make_shared<usb::Tp7MidiPort>(stack);
    backends.deviceMode = std::make_unique<EspTp7ModeSwitcher>(stack, midi);
    auto audio = std::make_unique<usb::Tp7AudioOutput>(stack);
    audio->setDeviceResetHook(
        [stack, midi, mtp = backends.mtp.get()] { resetTp7(stack, midi, *mtp); });
    backends.audioOutput = std::move(audio);
    backends.sendMidi = [midi](std::span<const std::uint8_t> packets) { return midi->send(packets); };
    return backends;
}

}  // namespace aw::platform::esp32p4
