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
  The same call sets priorities, which replace `SCHED_FIFO`. The platform
  makes that call through the placer it installs for `core::threads::start`.
  A task that uses the FPU stays on the core where it first used it, so a
  long-running float thread needs placing on purpose.
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

The probe settled the class question: UAC1, high-bandwidth.

**Decision (2026-10-14): monitor audio plays through the TP-7, as on the Pi.**
Bridge is used with the TP-7 as its audio interface, so local I2S is dropped
from the plan, and P3 becomes the USB playback path. P3a, below, is the probe
that de-risks it.

## P3a progress: TP-7 playback probe

`firmware/esp32p4/tp7-audio-probe` streams a 1 kHz tone to the TP-7's
playback endpoint, on channels 1–2 only. ESP-IDF v5.5.1 needed four things
from the caller, none of which needs a patched ESP-IDF:

- **`SET_INTERFACE`.** `usb_host_interface_claim` never sends it. Without it
  the TP-7 stays on alternate 0, drops the audio and never answers the
  feedback endpoint, while OUT packets still report "completed".
- **FIFO split.** The high-speed controller's FIFO is 896 lines. The default
  split gives periodic OUT 512 bytes and rejects the 882-byte endpoint. A
  custom split (`usb_host_config_t::fifo_settings_custom`: RX 320,
  non-periodic TX 128, periodic TX 448 lines) fits a whole 1764-byte
  microframe. The real backend needs the same split, so MTP's bulk IN shares
  an RX FIFO of 1280 bytes.
- **Multi-count and PID.** The driver never writes `HCCHAR.EC`, and starts
  every channel at `DATA0`. The probe writes `EC` = 2 and PID `MDATA` into the
  claimed playback channel before the first transfer (`EC` = 1, `DATA0` for
  feedback), as Linux's dwc2 does. This holds because the HAL never touches
  those fields after allocating the channel.
- **Feedback format.** The TP-7 reports 3-byte Q10.14 frames per
  millisecond, about `0x180000` (96.000). The probe sizes each packet from it.

On the bench, playback runs at 1000 packets/s and 96 000 frames/s with no
failed packets, and feedback arrives with no errors.

Recorded tests (the probe's automatic lab: it arms the TP-7 over USB-MIDI,
records, switches to MTP and fetches the take): coded packets and a
frame-numbering ramp arrived intact, but sines were muted in 8–11 ms
stretches, while squares were not. A Mac and a Raspberry Pi played the same
sine cleanly. The cause was the wiring between the Pico's USB pins and the TP-7
(a 200 mm JST lead into a USB-C socket, then a USB-C cable). Isochronous OUT
has no handshake, so the TP-7 silently drops a corrupted packet. With a new
cable (2026-10-17), four takes in a row recorded the −18 dBFS sine in 100% of
1 ms blocks, at −21.0 dB RMS, with no glitches, and packet sizes following the
feedback. Keep the USB data leads short and twisted.

## P3 progress: monitor audio through the TP-7

`Tp7AudioOutput` (`src/platform/esp32p4/usb/`) is the platform's audio
output, wrapped in `FallbackAudioOutput` as on the Pi: while the TP-7 is
unplugged or in MTP mode the engine runs on the silent wall clock, and the
app's 2-second `refresh()` switches to the TP-7 when it returns. The engine
stays at 48 kHz stereo; projects never change rate. The TP-7's playback
endpoint takes only 96 kHz, six channels, so the backend converts with the
Pi's `convertStereo48ToTp7Monitor96` (FL/FR carry the mix).

- A render task (priority 17, core 1) pulls one 256-frame period from the
  engine whenever the `FrameFifo` of packed 96 kHz frames holds under 1024
  frames. USB completions (3 transfers of 4 packets in flight) refill each
  packet with as many frames as the feedback asks for (`IsoPacketSizer`). The
  render side follows the FIFO level, so the engine runs on the TP-7's clock,
  which measures about 96.003 frames/ms against the P4's (30 ppm, ordinary
  crystal tolerance). Latency is about 23 ms (FIFO target plus queued
  transfers); a dry FIFO plays silence and reports an xrun.
- `EspUsbStack` now installs the FIFO split playback needs (RX 320,
  non-periodic TX 128, periodic TX 448 lines) and runs its host and client
  tasks at priorities 19 and 18, above the engine's streaming thread.
- `Tp7MidiPort` keeps the TP-7's MIDI interface claimed while it is attached,
  with a read always pending. The TP-7 sends MIDI clock while its transport
  runs; once that was queued unread, it ignored the SysEx that switches it to
  MTP. The mode switch and bench tooling (`EspUsbBackends::sendMidi`) send
  through it.
- Feedback watchdog. When its host disappears mid-stream (the P4 reset or lost
  power while the TP-7 stayed on), the TP-7 answers the feedback endpoint with
  empty packets until it re-enumerates. Bus resets and alternate-setting
  changes do not clear it (tried three times per boot, twice). Open-loop at
  96.000 its buffer reaches a limit after about 13 s and it slips a sample
  every 4 ms. So after 1.5 s without a rate, `refresh()` asks for a reset: the
  MTP SysEx, then a PTP session opened and closed, which returns the TP-7 to
  audio (at most twice in a row). On the bench that took about 4 s from boot
  to a locked stream.

Bench test: `firmware/esp32p4/tp7-output-test` plays a 660 Hz, −18 dBFS sine
from a 48 kHz callback through the real backend and runs the record-and-fetch
loop. Five takes in a row (one after a mid-stream reset, recovered by the
watchdog) recorded the sine in 100% of 1 ms blocks at −21.0 dB RMS with no
glitches. The bridge firmware on the bench went through the same recovery
on its own: silent clock at boot, TP-7 attached, reset, monitoring active.

Not yet verified: a real project's playback through the TP-7 by ear, and a
long session for xruns (the remaining P3 acceptance).

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

## P2 progress: input and display

The KY-040 is wired to the Pico with CLK on GPIO16, DT on GPIO15, SW on GPIO17,
and + on 3.3 V. A pin-finder firmware identified the pins on the bench: CLK
leads DT on clockwise turns. `EspGpioEncoder` and `EspGpioButtons` sample the
pins at 1 kHz from a priority-10 task. They reuse the Pi's `QuadratureDecoder`
and `ButtonStateMachine`, which now live in `aw/platform/InputDecoding.hpp`.
On the board, one clockwise click moved the home-screen highlight from
TRANSPORT to TRACKS, and a press opened the Tracks screen.

The same 7-pin SSD1306 module as the Pi's is wired with SCK on GPIO6, SDA
(MOSI) on GPIO5, RES on GPIO4, DC on GPIO3 and CS on GPIO14, with VCC on 3.3 V.
`EspSpiOledBus` drives it over SPI2 at 8 MHz in mode 0, under the unchanged
`Ssd1306Display` protocol code. It worked on the first flash.
`SerialPreviewDisplay` still mirrors each frame to the serial log, so a
capture shows what the panel shows. Storage (FatFS on SD) is the remaining P2
item.

## P2 progress: SD card

`FatFsStorage` mounts the card at `/sdcard`, through SDMMC on the P4 reference
pins and powered from LDO VO4. It never formats the card. On the bench it
mounted a 64 GB SanDisk card at 20 MHz. FatFS cannot rename onto an existing
file, so `rename()` replaces a file in three steps: move the target to
`.old`, move the new file in, delete `.old`. Any later access to that path
completes or rolls back an interrupted replace, so a power cut during a save
leaves either the new `metadata.json` or the previous one, never neither.
Because newlib's file offsets are 32-bit, a single file can be seeked only
within its first 2 GiB.

`StdioFile`, the `FILE*`-backed `hal::FileHandle`, moved from `PosixStorage`
to `aw/platform/StdioFile.hpp` so both backends share it.

Console writes now use `fileno(stdout)` rather than fd 1. With FatFS linked
in, `/dev/console` no longer had descriptor 1, and every log line silently
went nowhere.

## Storage and import speed

Importing a take was unusably slow: about 0.45 MiB/s, so a 200 MiB take took
7 minutes. `firmware/esp32p4/storage-bench` timed each stage. The card did
5.3 MiB/s from an aligned internal-RAM buffer but 0.48 MiB/s through
`FatFsStorage`. ESP-IDF's SD driver only uses multi-sector DMA when the
buffer's address and size are cache-aligned; otherwise it writes one 512-byte
sector per command through a bounce buffer. Import data arrives in PSRAM,
shifted by the 12-byte MTP container header, so almost every sector took that
path, and reads too.

`FatFsStorage` now hands out `FatFsFile`s (POSIX descriptors instead of
stdio) that move reads and writes of 4 KiB or more through one shared
64 KiB buffer in internal DMA RAM, 128-byte aligned. The data is placed at
the file offset modulo 128, so the whole sectors FatFS passes straight to the
driver start aligned. The card also runs at 40 MHz (high speed) now, falling
back to 20 MHz if a card refuses. `EspMtpDevice` sends the Still Image class
Device Reset before each OpenSession, so a transfer cut off by a P4 reset no
longer leaves the TP-7 answering nothing.

| Bench (SanDisk 64 GB) | Before | After |
| --- | --- | --- |
| SD write, 64 KiB chunks | 0.48 MiB/s | 6.3–6.6 MiB/s |
| SD read, 64 KiB chunks | 1.5 MiB/s | 11.1 MiB/s |
| MTP read from the TP-7 | — | 26.2 MiB/s |
| Import path, 198 MiB take | ≈7 min | 38–43 s (4.6–5.2 MiB/s), CRC-checked |

The import is now bound by the card: 85 % of its time is SD writes. Writes
under 4 KiB still go sector by sector (0.49 MiB/s at 1 KiB); nothing large
writes that way. Overlapping the MTP reads with the SD writes would save at
most the remaining 15 %.

## Export: out of memory, TP-7 left in MTP mode, speed (2026-10-17 to 25)

Reproduced over the serial console (keys typed on the console now act as the
panel: see `EspConsoleInput`; `firmware/esp32p4/bridge/tools/screen.py` sends
keys and prints the screen, every screen change is logged as `ui screen …`).

- **Out of memory.** With a 12-channel deck loaded, internal DMA RAM had
  238 KiB free but no 64 KiB blocks left for the MTP session's two transfers.
  `EspMtpDevice` now allocates them once at boot; the SD transfer buffer is
  allocated at mount; allocations of 4 KiB or more go to PSRAM
  (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`).
- **TP-7 stuck in MTP mode.** A failed session left nobody to close one.
  `MtpManager::release()` opens and closes a session when none is open; the
  Import and Export screens call it on exit, and the UI calls it whenever an
  MTP device appears with no transfer screen open (also a TP-7 left in MTP
  mode by a reset).
- **Looked stuck, was asking.** When the TP-7 already holds the export's
  name, the screen asks to replace it (Export confirms, Back cancels).
- **Speed.** A 3.5-minute 12-channel deck (366 MB) took 288 s to render and
  55 s to send. The render now opens each source once (it reopened the files
  per block and track); the build is -O2; an unedited deck is sent as its
  source WAV without rendering (`passthroughSource`; the log says why a deck
  needs a render, e.g. "track 1 is muted"). Timed at 2048-frame blocks: reading
  88 s, writing 122 s, mixing 21 s. Packed 24-bit conversion loops (both ways)
  were added to cut the read and write phases; on the board reading still
  took 87 s (2026-10-25).
- **Render streamed into the upload (2026-10-25).** A 24-bit WAV's size
  follows from the deck's length, so `ExportService` sends the header
  (`WavWriter::header`) and then mixes block by block as MTP asks for data.
  There is no temporary file, and nothing is written to the card. The same
  deck now exports in 154 s instead of about 286 s: reading sources 75 s,
  mixing and converting 47 s, USB 33 s. The two-step version also failed
  once on an SD read timeout while reading its render back. MTP has to
  receive the size SendObjectInfo announced, so after a failure or a cancel
  the rest is zeros and the object is then deleted from the TP-7.
- **Render ahead (2026-10-25).** A thread renders up to four blocks ahead
  of the upload, so reading and mixing overlap the USB transfers: 120 s.
  The render is now the whole cost (reading 72 s, mixing and converting
  47 s); the upload spends 81 s of it waiting.
- **Cache-sized blocks (2026-10-25): 88 s.** `storage-bench` showed the card
  reading the deck's WAV at 11.3 MiB/s in 72 KiB blocks, but the 24-bit
  conversion running three times slower in 2048-frame blocks than in 512
  (to float 18 vs 55 MiB/s, from float 14 vs 29), the same in PSRAM and in
  internal RAM: 2048 frames of 12 channels overflow the cache. The render
  now mixes 512-frame blocks, and `WavReader::setReadAhead` keeps each
  source read at one 64 KiB SD command. Reading 59 s, mixing and converting
  28 s; the upload still waits 53 s for the render.
- **Sources read in the background (2026-10-26): 60 s.** With
  `setReadAhead(bytes, true)` each source reads its next 64 KiB chunk on a
  thread of its own while the render mixes the current one. Mixing and
  converting 33 s; the render waits 26 s for chunks, the upload 18 s for
  the render. Four exports in a row: 60.2 to 62.3 s. Two problems came with it, both fixed:
  - *SD read timeouts.* With the card, USB and both cores busy at once,
    about one export in three failed on `sdmmc_read_sectors_dma ... status
    0x900`. FatFS returns what it read before the failure as a short read,
    then refuses every further call on that file, `lseek` included.
    `FatFsFile` now remembers where each read ended, reopens a read-only
    file there and repeats the read (up to three tries). The bench showed it
    recover mid-export.
  - *Task watchdog warnings every 5 s (IDLE0).* IDF FreeRTOS pins a task to
    the core it is on the first time it uses the FPU. The render does float
    mixing, so it stayed on core 0, the USB tasks' core, at about 80%. The
    read threads it woke filled the rest of the core. Per-task run-time stats
    showed core 1 idle 40% of the time. `core::threads::start(ThreadRole::
    Compute, ...)` lets the platform place such a thread; the P4 pins compute
    threads to core 1, which the TP-7 audio task only uses outside MTP mode.
    No warnings since, and the export got a little faster.
- Imports now save the project (an unsaved deck was lost at power-off and
  left its WAV on the card).

## P4 progress: TP-7 mode switch and MTP

Opening Import or Export on the P4 now switches the TP-7 into MTP mode on its
own, as on the Pi. The portable screens already called
`DeviceModeInterface::requestMtpMode()`; the P4 had no backend for it.
`createUsbBackends()` adds three backends that share one ESP-IDF USB host:
`USBInterface` for hotplug, `MTPInterface` (the probe's PTP code plus the
host-to-device data phase, for SendObjectInfo and SendObject), and the TP-7
SysEx mode switch over USB-MIDI. Hotplug callbacks run on their own task,
because `MtpManager` closes the session from inside the detach callback.

On the bench, Import worked twice in a row. The TP-7 dropped out of audio mode,
came back as `2367:0019 'TP-7 MTP Device'` at high speed, the session opened,
and the screen listed `library`, `memo` and `recordings`. Leaving the screen
closed the session, and the TP-7 returned to audio mode. Each return logs
`E HUB: Root port reset failed`, then succeeds on the retry about 0.6 s later.
Not yet exercised: copying a take (it needs the SD card), and Export (it needs
a project, so it also waits on storage).

## Apogee Duet 2 input types (2026-10-26)

Home → DUET INPUTS shows the type of each Duet input and changes it. Turn to
pick an input, then push. Turn to pick +4DBU, -10DBV, MIC or INST, then push
to send it; Back cancels. Only the confirmed type is sent, so the Duet never
switches through its microphone stage while the knob turns. The Duet replaces
the TP-7 on the P4's USB port for this.

- **Protocol.** Apogee's Maestro app sets everything with one-byte vendor
  requests on endpoint 0. take_control (github.com/stefanocoding/take_control)
  worked them out. The input type is request 22: read with request type 0xC0,
  write with 0x40, wValue 0, the input (0 or 1) as wIndex, and the value 0
  for +4 dBu, 1 for -10 dBV, 2 for Mic and 3 for Instrument. As Maestro does,
  `devices::ApogeeDuet` first ungroups the inputs (request 68 → 0 on both).
- **Two Duet 2s.** take_control targets the USB 2.0 compliant Duet 2 (product
  0x0016). The bench unit is the original Duet 2, 0c60:0007, and answers the
  same requests. Bridge tries the ID that last worked, then the other one.
- **Layers.** `hal::UsbControlInterface` is a narrow control-request call
  that is optional in `Platform`. On the P4, `EspUsbControl` leases the device
  for one request and uses the existing `usb::controlTransfer`. Endpoint 0
  belongs to no interface, so this works while a class driver streams the
  device's audio. The Pi has no backend yet: libusb could do it with
  `libusb_control_transfer`.
- **Bench.** The page read +4DBU, +4DBU. Setting input 1 to INST read back
  INST, and the Duet still reported INST after a Bridge reboot. Setting it
  back to +4DBU read back +4DBU. With no Duet, the page says NO DUET, and the
  log names what is attached instead.

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
| **P3a — TP-7 playback probe** (done; the mutes were the cable) | `firmware/esp32p4/tp7-audio-probe`: UAC1 high-bandwidth isochronous OUT with a custom FIFO split and multi-count | Clean 1 kHz tone from the TP-7; feedback endpoint reports a rate |
| **P3 — Audio out through the TP-7** (backend done, sine verified by recording; long session open) | `Tp7AudioOutput`: engine's 48 kHz stereo converted with the Pi's `convertStereo48ToTp7Monitor96`, rate-matched to the feedback endpoint, sharing the USB host with MTP | Six-lane playback with no xruns over a long session |
| P4 — MTP | `EspUsbHost`, `EspMtpDevice`, `Tp7UsbMidiModeSwitcher`, built from the probe | Phase 8 acceptance (`docs/phase8-tp7-test.md`) on the P4 |
