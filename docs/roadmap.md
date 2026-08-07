# Roadmap

Each phase ends with compilable code, tests, and a design write-up, then waits for
approval before the next begins.

## Phase 1 — Structure and architecture ✅

Project skeleton, HAL interfaces, placeholder classes for every subsystem, the
simulator backend, a runnable application shell, and 59 unit tests. Every
architectural decision is written down in `docs/architecture.md`.

## Phase 1.5 — Prototype I/O backends ✅

Real display and input for the bench hardware, so every later phase gets visual
feedback on the actual device as it lands rather than at the end.

- `Ssd1306Display` + `SpiOledBus` — 128×64 monochrome SPI OLED (SSD1306/SH1106),
  the prototype's primary display. RGB565 thresholded to 1bpp and packed into
  page RAM; real spidev + GPIO-chardev transport using only kernel uapi headers,
  no libgpiod. `aw-oled-test` proves wiring before any UI exists.
- `FramebufferDisplay` — the 3.5" HDMI LCD via `/dev/fb0` (the alternative
  display), 16 and 32 bpp, 320×240 canvas letterboxed inside 480×320.
- `EvdevTouch` — XPT2046 through the kernel, hit-testing a virtual key strip and
  emitting ordinary `ButtonId`s. Touch never reaches the UI. (HDMI LCD only.)
- `TerminalDisplay` / `KeyboardInput` — ANSI truecolour preview and key control,
  for working over SSH with no hardware at all.
- `CompositeButtons` — merges GPIO, touch and keyboard into one event stream.

See `docs/hardware.md` for wiring and setup. The KY-040 encoder itself is still
Phase 6 (it needs libgpiod); its pin map and decoding requirements are recorded.

> Note: the OLED is 128×64 monochrome, which diverges from the 320×240 colour
> ESP32-P4 target — Phase 7 will design a minimal UI for it. Chosen because it
> frees the GPIO header for the encoder, which the pin-hungry HDMI HAT does not.

## Phase 2 — WAV support

**Complete.**

`WavReader`, `WavWriter`, `WavMetadata`, `SampleConverter`.

- PCM, 24-bit, 48 kHz, one through twelve interleaved channels (mono through six
  TP-7 stereo pairs).
- Proper RIFF chunk-list parsing (the TP-7 and most recorders emit extra chunks).
- Streaming only — nothing loads a whole file.
- Tests against `MemoryStorage` with hand-built files, including deliberately
  malformed ones.
- RIFF and chunk sizes are checked against both the declared container and physical
  file bounds. Writer headers are checkpointed once per second of new audio and on
  close, keeping interrupted recordings playable without flushing every block.

## Phase 3 — Project manager

**Complete.** The metadata parser is a small purpose-built implementation with no
third-party dependency.

`Project`, `Track`, `Timeline`, `Clip`, `UndoStack`, `MetadataSerializer`,
`ProjectManager`. Create/open/save/close, atomic metadata writes, and one test per
command asserting `apply` → `revert` restores the exact prior state.

## Phase 4 — Audio engine

**Complete.**

Multi-track playback: gain, mute, solo, pan, loop, seeking, synchronisation. Streaming
thread with per-track ring buffers, scene swap at block boundaries, meters and
position posted as events. No DSP effects, no GUI dependencies. Punch-in recording
stays reserved, not implemented.

The product model now supports multiple decks with exactly one active for playback.
Inactive decks are source libraries. Clips route a mono channel or adjacent stereo
pair from an immutable multichannel TP-7 WAV into one of the active deck's six lanes.
Routes with an identical source range and placement share one multichannel reader and
bounded ring, so a six-pair TP-7 file is opened and streamed only once.

`Record` uses the confirmed Portastudio model: it arms a track; `Play` begins the
recording pass. Phase 4 reserves and tests that state transition only. Audio capture
and punch-in writing remain future work after the input backend exists.

## Phase 5 — ALSA backend

**Implemented; Raspberry Pi hardware acceptance pending.**

`S24_3LE`, 48 kHz, stereo, `SCHED_FIFO` thread, explicit period/buffer sizing, xrun
recovery and reporting. Six synchronized stereo project tracks are mixed by Phase 4
before the packed stereo bus reaches ALSA. A missing interface falls back to the silent
wall-clock output without changing transport timing.

The mandatory TP-7 monitor path is supported as a backend-only exception: its USB
descriptor exposes asynchronous `S24_3LE`, 96 kHz, six-channel playback. The Pi keeps
the engine callback at 48 kHz stereo, upsamples exactly 2x, routes it to FL/FR, and
writes silence to FC/LFE/RL/RR. Stored and MTP-exported audio is never resampled.

## Phase 6 — GPIO backend

libgpiod edge events, debounce, long-press, auto-repeat, quadrature decoding with
detent coalescing. Buttons produce events; no UI code inside GPIO classes.

## Phase 7 — Display

SPI panel driver, bitmap font generation, and the seven screens drawn for real:
Home, Project Browser, Track View, Playback, Import, Export, Settings — plus waveform
overviews, meters and the scrolling list.

## Phase 8 — MTP

libusb hotplug, libmtp sessions, folder enumeration, WAV listing, selected import and
export. Never synchronise automatically; never overwrite without confirmation.

## Later

- Punch-in recording.
- ESP32-P4 port: a new `src/platform/esp32p4/`, and nothing else.
