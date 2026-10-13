# ESP32-P4 port: feasibility and plan

**Verdict: feasible.** The architecture was built for this port, and that mostly
holds up. There is one hard problem: playing monitor audio *through the TP-7 over
USB*, as the Pi does. The rest of the work is ordinary backend code.

## What was checked

- **The portable layers compile for the P4 today.** That covers every `.cpp` in
  `core`, `events`, `storage`, `audio`, `mtp`, `display`, `project`, `ui` and `app`.
  They were compiled with Espressif's `riscv32-esp-elf-g++` 14.2 (ESP-IDF v5.5.1,
  `-march=rv32imafc -mabi=ilp32f`). `std::filesystem::path`, `std::jthread`,
  `std::mutex`, `std::condition_variable` and `std::variant` events all build. The
  only failure was a real portability bug, now fixed: on the ESP toolchain
  `std::int32_t` is `long`, so `std::clamp(int + int32_t, int, int)` could not
  deduce its type in three screens.
- **The ESP-IDF v5.5.1 USB host stack** (`components/usb`) has everything an MTP
  initiator needs: device open, interface claim, bulk transfers, hub support, and
  a choice of controller (`peripheral_map`, where BIT0 is high speed on the P4). It
  has **no MTP/PTP class driver**, so we write our own.
- **Espressif's host-side audio driver (`usb_host_uac`) supports UAC 1.0 only.**

## Subsystem by subsystem

| Subsystem | Raspberry Pi backend | ESP32-P4 replacement | Risk |
| --- | --- | --- | --- |
| Portable layers | — | Unchanged, built as an ESP-IDF component | Low (verified) |
| TP-7 detection | libusb hotplug | `usb_host` client events | Low |
| TP-7 → MTP mode | ALSA raw MIDI SysEx | Same SysEx as USB-MIDI packets on the MIDI OUT bulk endpoint | Low (in the probe) |
| MTP import/export | libmtp | Hand-written PTP/MTP initiator over bulk pipes, behind the existing 8-operation `MTPInterface` | Medium (read path in the probe) |
| Local storage | `std::filesystem` on ext4 | SD card over SDMMC 4-bit + FatFS through VFS | Low–medium, see notes |
| Display | spidev SSD1306 | `esp_lcd` / `spi_master` with DMA | Low |
| Encoder + buttons | libgpiod | GPIO ISR (or PCNT) + `esp_timer` debounce | Low |
| Monitor audio, local | ALSA | I2S DMA to a codec/DAC, 48 kHz 24-bit stereo | Low |
| Monitor audio, **through the TP-7** | ALSA `hw:TP7,0`, 96 kHz 6-ch | UAC1 host streaming over a **high-bandwidth** isochronous OUT endpoint (2 × 882 bytes per microframe) with an async feedback endpoint; needs a patched host controller driver | **High** |

### Notes that change code

- **The pthread stack defaults to 3 KiB on ESP-IDF.** P1 raises the default to
  16 KiB in `sdkconfig.defaults`, which is enough to boot. The streaming thread, I/O
  worker and UI need more. Call `esp_pthread_set_cfg` before creating each
  `std::thread` (from the platform layer, so the portable code stays unchanged).
  The same call sets priorities, which replace `SCHED_FIFO`.
- **Memory.** Each shared source stream holds 16 × 512 frames × 12 channels ×
  4-byte floats ≈ 384 KiB. Six differently placed streams come to about 2.3 MiB.
  That fits in PSRAM (P4 boards carry 16–32 MiB) but not in the 768 KiB of internal
  SRAM. Ring buffers go to PSRAM. DMA buffers (I2S, USB, SPI) stay in internal RAM.
- **FatFS rename does not replace an existing file.** `AtomicFileWriter` renames a
  temp file over `metadata.json`. The P4 storage backend must do
  remove-then-rename and recover a leftover temp file on open. That part is not
  atomic, so it must be designed explicitly.
- **FAT32 caps files at 4 GiB.** That matches the RIFF limit `WavWriter` already
  enforces. Enable long file names (`CONFIG_FATFS_LFN_HEAP`).
- `aw_platform_dev` stops the configure step on non-Linux hosts. The
  `esp32p4` branch of `src/platform/CMakeLists.txt` now returns before it is
  reached (P1).

## Bench results (P0, Waveshare ESP32-P4-Pico)

Measured on 2026-10-14 with TP-7 OS 1.1.11, connected to the Pico's high-speed
header through a 5-wire USB-C socket with 56 kΩ from CC to 5 V:

- **Attach and enumeration work.** The TP-7 attaches at `speed HIGH (480 Mbit/s)`
  about 0.4 s after boot. VBUS, the CC pull-up and the data wiring are fine.
- **The mode switch works.** The USB-MIDI SysEx makes the TP-7 drop off and come
  back as `2367:0019 'TP-7 MTP Device'` about 1.2 s later. After `CloseSession` it
  returns to audio mode on its own about 1 s later. On one of three runs that
  re-attach logged `E HUB: Root port reset failed` and then succeeded on the
  library's retry. The real backend must tolerate a failed first reset.
- **MTP works.** `OpenSession`, storage info, listing (`recordings` held 114
  objects) and `GetPartialObject` (0x101B) all succeed. `GetPartialObject64`
  and `GetObjectPropList` are not supported, so objects over 4 GiB cannot be read
  by offset. TP-7 takes stay below that, because of the RIFF limit.
- **Read throughput is 27.8–27.9 MiB/s** with 1 MiB `GetPartialObject` requests
  and a 64 KiB IN transfer buffer. That is about 17× real time for a 12-channel
  48 kHz take. MTP import speed will be limited by the SD card, not by USB.
- **The TP-7 is UAC1, not UAC2.** It offers three configurations, and ESP-IDF
  always activates the first. All three are UAC1 with the same streaming
  interfaces: 6 channels, 24-bit in 3 bytes, a single 96 kHz rate, isochronous
  IN `0x85` and OUT `0x06` (asynchronous, with feedback on `0x83`). Each data
  endpoint is **high-bandwidth**: `wMaxPacketSize` = 882 with one additional
  transaction per microframe, so 1764 bytes per 1 ms service interval. That is
  98 frames at 96 kHz. ESP-IDF's descriptor printer drops the multiplier bits
  and shows only 882. The probe now prints the real figure.

## The monitor-audio decision

On the Pi, Bridge plays through the TP-7 as a USB sound card at 96 kHz, 6 channels.
The probe shows this is UAC1, which removes the need for a UAC2 class driver. A
different problem replaces it: the endpoint needs **two isochronous transactions
per microframe**. ESP-IDF v5.5.1's DWC host driver (`hcd_dwc.c`) never reads the
multiplier (`USB_EP_DESC_GET_MULT` is defined but not used). It schedules one
transaction per interval, which is half the bandwidth this stream needs. So
playing through the TP-7 means patching the host controller driver as well as
writing the isochronous streaming and feedback rate matching. Also, the TP-7
cannot be in audio mode and MTP mode at once. Two options:

1. **Recommended: local I2S output** (headphone or line codec on the board).
   This is what `docs/architecture.md` already assumes ("I2S instead of ALSA"). It
   is low risk and the I2S pull model maps directly onto `AudioOutputInterface`.
2. **TP-7 over USB, as on the Pi.** Possible, but it is the single largest and
   riskiest work item in the port. Defer it until everything else works.

The probe settled the class question: UAC1, high-bandwidth. The recommendation
stays local I2S, and USB monitoring stays deferred.

## P1 result

`firmware/esp32p4/bridge` builds every portable layer unchanged, through the
repository's own per-layer CMake targets with `AW_PLATFORM=esp32p4`. The image
is about 390 KiB, and the build has no warnings under ESP-IDF's `-Werror=all`.
On the Pico it reaches `app_main` 1.8 s after reset, initialises the whole
application (engine on `NullAudioOutput`, null storage, input and USB), and
prints the home screen to the serial log through the real SSD1306 packing code.
There are 472 KiB of internal RAM and 32 MiB of PSRAM free. Three changes to
shared code were needed:

- `ProjectMetadata.cpp` parsed floats with `std::stof` inside `try`/`catch`.
  That does not compile with `-fno-exceptions`, so it now uses `strtof`.
- `PlaybackScreen.cpp` formatted the transport time into 12 bytes, which
  truncates past 99 minutes. GCC 14 rejects this under ESP-IDF's
  `-Werror=all`. The buffer is now 32 bytes.
- `AwCompilerOptions` warning flags are now C++-only, and `AW_INCLUDE_DIR` no
  longer assumes the repository is the top-level CMake project.

## P2 progress: input

The KY-040 is wired to the Pico with CLK on GPIO16, DT on GPIO15, SW on GPIO17,
and + on 3.3 V. A pin-finder firmware identified the pins on the bench: CLK
leads DT on clockwise turns. `EspGpioEncoder` and `EspGpioButtons` sample the
pins at 1 kHz from a priority-10 task. They reuse the Pi's `QuadratureDecoder`
and `ButtonStateMachine`, which now live in `aw/platform/InputDecoding.hpp`.
On the board, one clockwise click moved the home-screen highlight from
TRANSPORT to TRACKS, and a press opened the Tracks screen.

## If a TP-7 does not connect

On the bench the TP-7 connected on the first attempt (see above). When it does
not, these are the usual causes, most likely first. Each is explained in
`firmware/esp32p4/tp7-probe/README.md`:

1. No 5 V VBUS on the P4's host socket, which some boards gate behind a GPIO or a
   power option.
2. The TP-7 is on the full-speed or device-only connector instead of the
   high-speed host one.
3. `CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE` is still at its 256-byte default.
   The TP-7's composite audio/MIDI descriptor is larger, and enumeration fails.
4. An on-board hub without `CONFIG_USB_HOST_HUBS_SUPPORTED`.

## Plan

Each step ends with something to run on the bench, the same way the Pi phases did.

| Step | Delivers | Bench acceptance |
| --- | --- | --- |
| **P0 — USB probe** (done, passed on the bench) | `firmware/esp32p4/tp7-probe` | TP-7 enumerates at high speed, switches to MTP, session opens, read throughput measured |
| **P1 — Firmware skeleton** (done, boots on the bench) | ESP-IDF project in `firmware/esp32p4/bridge` building all portable layers as a component; `AW_PLATFORM=esp32p4`; null backends; `app_main` composition root | Boots; UI renders to the serial log |
| P2 — Storage, display, input | `FatFsStorage`, SSD1306 over `esp_lcd`, encoder/buttons over GPIO | Create/open/save projects; navigate the UI on the OLED |
| P3 — Audio out | `I2sAudioOutput` (48 kHz, 24-bit, 256-frame periods) | Six-lane playback with no xruns over a long session |
| P4 — MTP | `EspUsbHost`, `EspMtpDevice`, `Tp7UsbMidiModeSwitcher`, built from the probe | Phase 8 acceptance (`docs/phase8-tp7-test.md`) on the P4 |
| Later | UAC1 high-bandwidth monitoring through the TP-7, if still wanted | — |
