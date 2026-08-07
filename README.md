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
**Target platform:** ESP32-P4. The architecture is platform-independent from day one —
only `src/platform/` changes when the port happens.

## Status

**Phases 1–4 complete.** The portable architecture, Raspberry Pi prototype I/O,
streaming WAV layer, project manager, non-destructive model, and realtime multitrack
playback engine are implemented. Playback uses bounded local-storage buffers; the TP-7
is exclusively an explicit MTP import/export endpoint and is never a live audio source.
Every project contains six synchronized stereo tracks (twelve source channels) mixed
to one stereo monitor/output bus.

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

## Layout

```
include/aw/       public headers, one directory per module
  core/           vocabulary types: Result, Error, ring buffer, formats
  hal/            hardware interfaces (buttons, encoder, display, audio, storage, USB, MTP)
  events/         event types, lock-free queues, the bus
  audio/          WAV I/O, transport, mixer, engine
  storage/        project layout, atomic writes
  project/        data model, undo, import/export services
  mtp/            device connection and object-tree browsing
  display/        framebuffer, painter, font
  ui/             screens and navigation
  platform/       backend implementations (posix, simulator, rpi)
  app/            composition root
src/              implementations, mirroring the above
tests/            unit tests + fakes; links no hardware code
assets/           fonts and other compiled-in resources
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
