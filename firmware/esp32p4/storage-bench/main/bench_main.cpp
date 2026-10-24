///
/// Storage bench: where an import's time goes.
///
/// 1. SD writes through FatFsStorage (what ImportService uses) at several
///    chunk sizes, and raw POSIX writes as the card's ceiling.
/// 2. SD reads (what playback streams).
/// 3. MTP reads from the TP-7 into memory (EspMtpDevice::readObject).
/// 4. The import path itself: MTP read straight into an SD file.
///
#include <fcntl.h>
#include <unistd.h>

#include <cinttypes>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "aw/core/Logger.hpp"
#include "aw/platform/esp32p4/EspConsole.hpp"
#include "aw/platform/esp32p4/EspUsb.hpp"
#include "aw/platform/esp32p4/FatFsStorage.hpp"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

using namespace aw;
constexpr std::uint16_t kTeenageEngineering = 0x2367;
constexpr std::size_t kTestBytes = 4u << 20;

void line(const std::string& text) { platform::esp32p4::consoleWrite("BENCH " + text + "\n"); }

std::string rate(std::uint64_t bytes, std::int64_t micros) {
    char text[96];
    std::snprintf(text, sizeof text, "%.1f MiB in %.2f s = %.2f MiB/s", bytes / 1048576.0,
                  micros / 1e6, bytes / 1048576.0 / (micros / 1e6));
    return text;
}

void halWrite(hal::StorageInterface& storage, const core::Path& path, std::size_t chunk) {
    std::vector<std::byte> data(chunk, std::byte{0x5A});
    (void)storage.remove(path);
    const std::int64_t start = esp_timer_get_time();
    auto file = storage.open(path, hal::OpenMode::Write);
    if (!file) return line("open failed: " + file.error().describe());
    for (std::size_t done = 0; done < kTestBytes; done += chunk) {
        if (auto written = file.value()->write(data); !written) return line("write failed");
    }
    (void)file.value()->flush();
    file.value().reset();
    line("SD write, FatFsStorage, " + std::to_string(chunk >> 10) + " KiB chunks: " +
         rate(kTestBytes, esp_timer_get_time() - start));
}

void posixWrite(const core::Path& path, std::size_t chunk) {
    auto* data = static_cast<std::uint8_t*>(heap_caps_malloc(chunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (data == nullptr) return line("no DMA buffer");
    std::fill_n(data, chunk, 0x5A);
    ::unlink(path.c_str());
    const std::int64_t start = esp_timer_get_time();
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    for (std::size_t done = 0; fd >= 0 && done < kTestBytes; done += chunk) (void)::write(fd, data, chunk);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
    line("SD write, POSIX, " + std::to_string(chunk >> 10) + " KiB internal-RAM chunks: " +
         rate(kTestBytes, esp_timer_get_time() - start));
    heap_caps_free(data);
}

void halRead(hal::StorageInterface& storage, const core::Path& path, std::size_t chunk) {
    std::vector<std::byte> data(chunk);
    const std::int64_t start = esp_timer_get_time();
    auto file = storage.open(path, hal::OpenMode::Read);
    if (!file) return line("open failed");
    std::uint64_t total = 0;
    for (;;) {
        auto got = file.value()->read(data);
        if (!got || got.value() == 0) break;
        total += got.value();
    }
    line("SD read, FatFsStorage, " + std::to_string(chunk >> 10) + " KiB chunks: " +
         rate(total, esp_timer_get_time() - start));
}

std::optional<hal::UsbDeviceInfo> findTp7(hal::USBInterface& usb, bool mtp) {
    auto devices = usb.enumerate();
    if (!devices) return std::nullopt;
    for (const auto& device : devices.value())
        if (device.vendorId == kTeenageEngineering && device.supportsMtp == mtp) return device;
    return std::nullopt;
}

}  // namespace

extern "C" void app_main(void) {
    platform::esp32p4::lockIdfLogging();
    core::log::installSink(std::make_shared<platform::esp32p4::EspConsoleLogSink>());
    vTaskDelay(pdMS_TO_TICKS(2000));  // Time for the Mac to open the console.

    auto storage = platform::esp32p4::FatFsStorage::mount({}, "/sdcard/projects");
    if (!storage) return line("no SD card: " + storage.error().describe());
    hal::StorageInterface& sd = *storage.value();
    const core::Path testFile{"/sdcard/bench.bin"};

    line("starting");
    posixWrite(testFile, 64u << 10);
    for (const std::size_t chunk : {std::size_t{64} << 10, std::size_t{8} << 10, std::size_t{1} << 10})
        halWrite(sd, testFile, chunk);
    for (const std::size_t chunk : {std::size_t{64} << 10, std::size_t{512} << 10}) halRead(sd, testFile, chunk);
    (void)sd.remove(testFile);

    auto usb = platform::esp32p4::createUsbBackends();
    (void)usb.usb->start();
    std::optional<hal::UsbDeviceInfo> device;
    for (int i = 0; i < 100 && !device; ++i) {
        device = findTp7(*usb.usb, true);
        if (!device && findTp7(*usb.usb, false)) {
            (void)usb.deviceMode->requestMtpMode();
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!device) return line("no TP-7");
    if (auto status = usb.mtp->openSession(device->key()); !status) return line("MTP: " + status.error().describe());

    // The largest recording under 200 MiB, as a realistic import.
    std::optional<hal::MtpObjectInfo> take;
    auto storages = usb.mtp->storages();
    if (storages && !storages.value().empty()) {
        const auto id = storages.value().front().id;
        if (auto root = usb.mtp->listObjects(id, hal::kMtpRootHandle))
            for (const auto& folder : root.value())
                if (folder.isFolder && folder.name == "recordings")
                    if (auto files = usb.mtp->listObjects(id, folder.handle))
                        for (const auto& file : files.value())
                            if (!file.isFolder && file.sizeBytes < (200u << 20) &&
                                (!take || file.sizeBytes > take->sizeBytes))
                                take = file;
    }
    if (!take) return line("no recording to read");
    line("TP-7 file: " + take->name + ", " + std::to_string(take->sizeBytes >> 20) + " MiB");

    std::uint64_t chunks = 0;
    std::uint64_t total = 0;
    std::int64_t start = esp_timer_get_time();
    auto readStatus = usb.mtp->readObject(take->handle, [&](std::span<const std::byte> chunk) {
        ++chunks;
        total += chunk.size();
        return true;
    }, {});
    if (!readStatus) line("MTP read failed: " + readStatus.error().describe());
    line("MTP read to memory: " + rate(total, esp_timer_get_time() - start) + ", " +
         std::to_string(total / std::max<std::uint64_t>(chunks, 1) >> 10) + " KiB per sink call");

    // The import path: each MTP chunk written straight to an SD file.
    const core::Path copy{"/sdcard/import-bench.wav"};
    (void)sd.remove(copy);
    std::int64_t mtpMicros = 0;
    std::int64_t sdMicros = 0;
    total = 0;
    start = esp_timer_get_time();
    auto file = sd.open(copy, hal::OpenMode::Write);
    std::int64_t lastReturn = start;
    std::uint32_t sourceCrc = 0;
    readStatus = usb.mtp->readObject(take->handle, [&](std::span<const std::byte> chunk) {
        const std::int64_t arrived = esp_timer_get_time();
        mtpMicros += arrived - lastReturn;
        sourceCrc = esp_rom_crc32_le(sourceCrc, reinterpret_cast<const std::uint8_t*>(chunk.data()),
                                     static_cast<std::uint32_t>(chunk.size()));
        (void)file.value()->write(chunk);
        total += chunk.size();
        lastReturn = esp_timer_get_time();
        sdMicros += lastReturn - arrived;
        return true;
    }, {});
    if (!readStatus) line("MTP read failed: " + readStatus.error().describe());
    (void)file.value()->flush();
    file.value().reset();
    line("import path (MTP -> SD): " + rate(total, esp_timer_get_time() - start) + "; waiting on MTP " +
         std::to_string(mtpMicros / 1000) + " ms, writing SD " + std::to_string(sdMicros / 1000) + " ms");
    // Integrity: the copy read back from the card must match what MTP sent.
    std::uint32_t copyCrc = 0;
    std::uint64_t copyBytes = 0;
    if (auto back = sd.open(copy, hal::OpenMode::Read)) {
        std::vector<std::byte> data(48 * 1024 + 7);  // An odd size, to exercise unaligned reads.
        for (;;) {
            auto got = back.value()->read(data);
            if (!got || got.value() == 0) break;
            copyCrc = esp_rom_crc32_le(copyCrc, reinterpret_cast<const std::uint8_t*>(data.data()),
                                       static_cast<std::uint32_t>(got.value()));
            copyBytes += got.value();
        }
    }
    char check[128];
    std::snprintf(check, sizeof check, "integrity: MTP crc %08" PRIx32 ", SD copy crc %08" PRIx32
                  " (%" PRIu64 " bytes): %s", sourceCrc, copyCrc, copyBytes,
                  sourceCrc == copyCrc && copyBytes == total ? "MATCH" : "MISMATCH");
    line(check);
    (void)sd.remove(copy);
    usb.mtp->closeSession();
    line("done");
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
