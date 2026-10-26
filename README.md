# Audio Workstation

A standalone TP-7 companion for multitrack editing. Not a DAW — a dedicated device with
physical controls, a small screen, instant boot, and a fast workflow, in the spirit of
the Tascam Portastudio and the Zoom R-series.

It is designed to work with the **Teenage Engineering TP-7**, which it talks to over
**MTP** (the TP-7 exposes no mass storage). Recordings are imported to local storage,
edited non-destructively there, and selected tracks are written back on request. The
TP-7 is never used as a live playback source and is never modified without an explicit
confirmation.

**Prototype platform:** Raspberry Pi 5, C++20, CMake.
**Target platform:** ESP32-P4 (Waveshare ESP32-P4-Pico), ESP-IDF 5.5. The same
portable code runs on both; only `src/platform/` differs.

## Status

**Phases 1–8 complete.** The portable architecture, Raspberry Pi prototype I/O,
streaming WAV layer, project manager, non-destructive model, and realtime multitrack
playback engine are implemented. Playback uses bounded local-storage buffers; the TP-7
is exclusively an explicit MTP import/export endpoint and is never a live audio source.
Each deck contains six synchronized stereo-capable tracks. Exactly one deck is active
for playback; the others are source libraries. A TP-7 WAV may contain from one mono
channel through six stereo pairs (twelve interleaved channels), and clips reference a
pair without duplicating the imported file. All routes over the same placement share
one file reader and one bounded multichannel stream.

**ESP32-P4 port.** The whole application runs on the P4. It has an SSD1306 OLED
(mirrored to the serial log), a KY-040 encoder, projects on an SD card, and monitor
audio through the TP-7's USB playback endpoint. Import and Export go to the TP-7
over a hand-written MTP initiator. Export renders the deck straight into the
upload: a 3.5-minute, twelve-channel deck takes about 60 s. Progress, bench
results and decisions are in [docs/esp32p4-port.md](docs/esp32p4-port.md).

**Apogee Duet 2 inputs (ESP32-P4).** With a Duet 2 on the P4's USB port in place
of the TP-7, Home → **DUET INPUTS** shows each input's type and switches it
between +4 dBu, −10 dBV, Mic and Instrument. Turn to pick an input, push, turn
to pick the type, push to send it; Back cancels. Both Duet 2 revisions
(`0c60:0007` and `0c60:0016`) work. The vendor requests come from
[take_control](https://github.com/stefanocoding/take_control).

## Quick start

**Deploying to a Raspberry Pi for the first time?** Follow
**[docs/deploy-from-mac.md](docs/deploy-from-mac.md)** — a complete walkthrough from a
brand-new Pi in the box to the app running, assuming no prior Raspberry Pi experience.

Building locally:

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
AW_PROJECTS_ROOT=./projects ./build/src/app/audio-workstation
```

No third-party dependencies in Phase 1. See [docs/build.md](docs/build.md) for the
options.

Building and flashing the ESP32-P4 firmware (see
[firmware/esp32p4/bridge/README.md](firmware/esp32p4/bridge/README.md)):

```bash
. ~/esp/esp-idf/export.sh            # ESP-IDF v5.5.x
cd firmware/esp32p4/bridge
idf.py set-target esp32p4            # once
idf.py -p /dev/cu.usbmodem… flash monitor
```

On the P4, the serial console works as a keyboard: `j`/`k` turn the encoder, Enter
pushes it, and `b` is Back.

### Raspberry Pi service

When Bridge is installed to start automatically at boot, use these commands to check
its status, follow its logs, restart it, or stop it before running the program manually:

```bash
systemctl status audio-workstation
journalctl -u audio-workstation -f
sudo systemctl restart audio-workstation
sudo systemctl stop audio-workstation
```

## Layout

```
include/aw/       public headers, one directory per module
  core/           vocabulary types: Result, Error, ring buffer, formats
  hal/            hardware interfaces (buttons, encoder, display, audio, storage, USB,
                  USB control requests, MTP)
  events/         event types, lock-free queues, the bus
  audio/          WAV I/O, transport, mixer, engine
  storage/        project layout, atomic writes
  project/        data model, undo, import/export services
  mtp/            device connection and object-tree browsing
  devices/        settings of external USB devices (Apogee Duet 2 input types)
  display/        framebuffer, painter, font
  ui/             screens and navigation
  platform/       backend implementations (posix, simulator, rpi, esp32p4)
  app/            composition root
src/              implementations, mirroring the above
tests/            unit tests + fakes; links no hardware code
assets/           fonts and other compiled-in resources
firmware/esp32p4/ the P4 firmware (bridge) and its bench probes
docs/             architecture, build, roadmap
```

## Project format

Editing is non-destructive. Source WAVs are written once, at import, and never
modified.

```
Project/
  audio/          immutable source recordings
  edits/          rendered/bounced material
  undo/           serialised undo journal
  metadata.json   tracks, clips, mix, timeline
```

## Documentation

- **[docs/architecture.md](docs/architecture.md)** — every architectural decision, the
  alternatives considered, and what each choice costs. Start here.
- **[docs/deploy-from-mac.md](docs/deploy-from-mac.md)** — step-by-step: unboxing a Pi 5
  through to running the app, from a Mac. Start here if the Pi is new.
- **[docs/hardware.md](docs/hardware.md)** — prototype wiring (HDMI LCD, XPT2046 touch,
  KY-040 encoder), setup, and the keyboard control map.
- **[docs/build.md](docs/build.md)** — building, testing, options, sanitisers.
- **[docs/roadmap.md](docs/roadmap.md)** — the eight phases and what each delivers.
- **[docs/esp32p4-port.md](docs/esp32p4-port.md)** — the ESP32-P4 port: feasibility,
  progress, bench results (export speed, SD reliability, Duet control) and the plan.
