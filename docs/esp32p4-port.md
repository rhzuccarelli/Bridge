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
| Monitor audio, **through the TP-7** | ALSA `hw:TP7,0`, 96 kHz 6-ch | Custom **UAC2** host driver: high-speed isochronous OUT with an async feedback endpoint | **High** |

### Notes that change code

- **The pthread stack defaults to 3 KiB on ESP-IDF.** The streaming thread, I/O
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
- `aw_platform_dev` stops the configure step on non-Linux hosts. It has to be
  gated when the ESP-IDF build includes the tree.

## The monitor-audio decision

On the Pi, Bridge plays through the TP-7 as a USB sound card at 96 kHz, 6 channels.
Doing the same on the P4 means writing a UAC2 host driver, with isochronous
streaming and feedback-endpoint rate matching, on a USB stack that ships only UAC1
support. Also, the TP-7 cannot be in audio mode and MTP mode at once. Two options:

1. **Recommended: local I2S output** (headphone or line codec on the board).
   This is what `docs/architecture.md` already assumes ("I2S instead of ALSA"). It
   is low risk and the I2S pull model maps directly onto `AudioOutputInterface`.
2. **TP-7 over USB, as on the Pi.** Possible, but it is the single largest and
   riskiest work item in the port. Defer it until everything else works.

The probe reports whether the TP-7 is UAC1 or UAC2 (`summary: audio=...`). That
answer, together with which output you want, settles this decision.

## Why the TP-7 probably did not connect

These are the usual causes, most likely first. Each is explained in
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
| **P0 — USB probe** (done) | `firmware/esp32p4/tp7-probe` | TP-7 enumerates at high speed, switches to MTP, session opens, read throughput measured |
| P1 — Firmware skeleton | ESP-IDF project in `firmware/esp32p4/bridge` building all portable layers as a component; `AW_PLATFORM=esp32p4`; null backends; `app_main` composition root | Boots; UI renders to the serial log |
| P2 — Storage, display, input | `FatFsStorage`, SSD1306 over `esp_lcd`, encoder/buttons over GPIO | Create/open/save projects; navigate the UI on the OLED |
| P3 — Audio out | `I2sAudioOutput` (48 kHz, 24-bit, 256-frame periods) | Six-lane playback with no xruns over a long session |
| P4 — MTP | `EspUsbHost`, `EspMtpDevice`, `Tp7UsbMidiModeSwitcher`, built from the probe | Phase 8 acceptance (`docs/phase8-tp7-test.md`) on the P4 |
| Later | UAC2 monitoring through the TP-7, if still wanted | — |
