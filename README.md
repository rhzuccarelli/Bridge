# Audio Workstation

A standalone hardware multitrack recorder/editor. Not a DAW — a dedicated device with
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

**Phase 1 complete: structure, architecture and placeholders.** The tree builds, the
application runs against a simulator backend, and 59 unit tests pass. The audio, WAV,
project, MTP and display *implementations* are Phase 2–8 work; their stubs return
`NotImplemented` so a half-wired build fails loudly rather than silently doing nothing.

## Quick start

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/tests/aw_tests
AW_PROJECTS_ROOT=./projects ./build/src/app/audio-workstation
```

No third-party dependencies in Phase 1. See [docs/build.md](docs/build.md) for the
Raspberry Pi build and the available options.

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
- **[docs/build.md](docs/build.md)** — building, testing, options, sanitisers.
- **[docs/roadmap.md](docs/roadmap.md)** — the eight phases and what each delivers.
