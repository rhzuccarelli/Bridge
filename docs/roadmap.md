# Roadmap

Each phase ends with compilable code, tests, and a design write-up, then waits for
approval before the next begins.

## Phase 1 — Structure and architecture ✅

Project skeleton, HAL interfaces, placeholder classes for every subsystem, the
simulator backend, a runnable application shell, and 59 unit tests. Every
architectural decision is written down in `docs/architecture.md`.

## Phase 2 — WAV support

`WavReader`, `WavWriter`, `WavMetadata`, `SampleConverter`.

- PCM, 24-bit, 48 kHz, mono and stereo.
- Proper RIFF chunk-list parsing (the TP-7 and most recorders emit extra chunks).
- Streaming only — nothing loads a whole file.
- Tests against `MemoryStorage` with hand-built files, including deliberately
  malformed ones.

## Phase 3 — Project manager

`Project`, `Track`, `Timeline`, `Clip`, `UndoStack`, `MetadataSerializer`,
`ProjectManager`. Create/open/save/close, atomic metadata writes, and one test per
command asserting `apply` → `revert` restores the exact prior state.

*Needs a decision first: hand-rolled JSON vs a library — see architecture §Open
questions.*

## Phase 4 — Audio engine

Multi-track playback: gain, mute, solo, pan, loop, seeking, synchronisation. Streaming
thread with per-track ring buffers, scene swap at block boundaries, meters and
position posted as events. No DSP effects, no GUI dependencies. Punch-in recording
stays reserved, not implemented.

*Needs a decision first: `Record` semantics — Portastudio arm-then-play, or
field-recorder roll-immediately.*

## Phase 5 — ALSA backend

`S24_3LE`, 48 kHz, stereo, `SCHED_FIFO` thread, explicit period/buffer sizing, xrun
recovery and reporting.

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
