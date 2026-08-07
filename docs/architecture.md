# Architecture

This document explains *why* the code is shaped the way it is. The headers explain
what each class does; this explains the decisions behind them, the alternatives that
were considered, and what each choice costs.

The single constraint that drives everything: **the Raspberry Pi 5 is a prototype,
the ESP32-P4 is the product.** Every decision below is judged first on "does this
survive the port", second on "is it testable", third on "is it simple".

---

## 1. Layering

```
                 +---------------------------+
                 |            ui             |  screens, navigation, input routing
                 +---------------------------+
                    |          |         |
       +------------+          |         +--------------+
       v                       v                        v
+--------------+       +---------------+        +---------------+
|   project    |       |    display    |        |      mtp      |
| model, undo, |       | framebuffer,  |        | connection,   |
| import/export|       | painter, font |        | browsing      |
+--------------+       +---------------+        +---------------+
       |                                                |
       +----------------+-------------------------------+
                        v
                 +-------------+     +-------------+
                 |    audio    |     |   storage   |
                 | WAV, engine |     | layout,     |
                 |             |     | atomic write|
                 +-------------+     +-------------+
                        |                   |
                        +---------+---------+
                                  v
                          +---------------+
                          |    events     |
                          +---------------+
                                  v
                          +---------------+
                          |      hal      |  pure interfaces
                          +---------------+
                                  v
                          +---------------+
                          |     core      |  vocabulary types
                          +---------------+

                          +---------------+
                          |   platform    |  implements hal (rpi | simulator)
                          +---------------+
                                  ^
                          +---------------+
                          |      app      |  composition root: builds everything
                          +---------------+
```

Each layer is a separate CMake target that links only the layers below it. This is
not documentation — it is enforced. `aw_audio` does not link `aw_project`, so a
well-meaning change that makes the audio engine read a `Project` fails to compile
rather than quietly creating a cycle.

**Why layered libraries instead of one library with directories?** Directories are a
suggestion; link dependencies are a rule. The cost is a slightly longer
`CMakeLists.txt` per module. The benefit is that the test executable links only
portable modules — which is the mechanical proof that no business logic depends on
hardware.

---

## 2. Hardware abstraction: virtual interfaces, injected at the root

Every hardware capability is an abstract base class in `include/aw/hal/`:
`ButtonInterface`, `EncoderInterface`, `DisplayInterface`, `AudioOutputInterface`,
`AudioInputInterface`, `StorageInterface`, `USBInterface`, `MTPInterface`,
`ClockInterface`. A backend implements them; `hal::Platform` bundles the instances;
`app::Application` receives that bundle. There is not a single `#ifdef` outside
`src/platform/` and `src/app/main.cpp`.

**Alternatives considered:**

| Approach | Why not |
| --- | --- |
| `#ifdef` inside business logic | Untestable, and the ESP32 port becomes an archaeology exercise. Rejected outright. |
| Compile-time policies (templates) | Zero virtual-call overhead and appealing for a microcontroller. Rejected: every module becomes a template, compile times balloon, error messages become unreadable, and test doubles require instantiating the whole application at a new type. |
| Link-time substitution (same header, different .cpp) | Cheapest at runtime, but you cannot have two implementations in one binary — so a test cannot use a fake while the app uses the real one. Rejected. |

**What virtual dispatch actually costs here.** The concern is the audio path. In this
design the audio callback crosses *one* virtual boundary per block
(`AudioRenderCallback::renderAudio`, once per 256 frames), and everything inside the
mixer is concrete. That is one indirect call per ~5.3 ms — unmeasurable. The other
interfaces are called at UI rate or slower. So the abstraction is free where it
matters and cheap where it does not.

**Interface design rule.** Each interface is sized to what the *weakest* platform can
implement. `MTPInterface` has eight operations, not thirty, because on the ESP32-P4
someone will hand-write an MTP initiator over TinyUSB; a fat interface would force
that implementation to fake operations it does not have.

---

## 3. Threading model

Fixed now, in Phase 1, because it is the decision everything else is built on.

| Context | What runs there | Constraints |
| --- | --- | --- |
| **Audio thread** | `AudioEngine::renderAudio`, `Mixer`, `TrackPlayer::render`, `Transport::advance` | No allocation, no locks, no I/O, no logging, no exceptions. |
| **Streaming thread** | `TrackPlayer::stream`, WAV reads, retired-scene reclamation | Blocking file I/O, fills per-track ring buffers. |
| **I/O worker** | MTP transfers, project save/load, directory scans | Blocking, seconds-long operations. Cancellable. |
| **Main loop** | Event dispatch, UI update, render, display flush | Never blocks on anything. |

They communicate in exactly two ways:

1. **`core::RingBuffer`** — wait-free SPSC, for anything crossing the audio boundary.
2. **`events::EventQueue` → `events::EventBus`** — for everything else. Each producer
   owns a queue; the main loop drains them all and fans out to subscribers on one
   thread.

Consequence: **no mutex appears anywhere in the UI or project layers.** They are
single-threaded by construction. That is not an accident — it is why a screen can be
written without thinking about concurrency at all.

This maps cleanly onto FreeRTOS: each context becomes a task, the ring buffers become
the same ring buffers, and the priorities become explicit rather than inherited from
Linux scheduling policy.

### Why events are a `std::variant`, not a class hierarchy

`events::Event` is a closed `std::variant` of trivially-copyable POD structs, with a
`static_assert` enforcing that. It is what lets an event pass through a lock-free
queue with no allocation and no lifetime question. The cost is that adding an event
type edits a shared header — which is arguably a feature, since it forces a moment's
thought about who should handle it.

The `static_assert` is deliberately loud: the first person who adds a `std::string`
to an event gets a compile error explaining what to do instead (post an identifier,
let the consumer look the payload up).

---

## 4. Non-destructive editing

```
Project/
  audio/          immutable. Written once at import, read-only thereafter.
  edits/          rendered/bounced output (export renders here first)
  undo/           serialised command journal
  metadata.json   the project: tracks, clips, mix, timeline
```

A `Clip` names a source file, a range within it, and a position on the timeline.
Trimming moves an integer. Splitting creates a second `Clip` over the same file.
Deleting removes a `Clip`. **No edit ever touches a byte of audio.**

**Alternative: edit files in place, keep undo copies.** Simpler to implement, much
worse to live with — every edit costs a full file rewrite on an SD card, undo costs
another, and a power cut mid-edit corrupts the take. Rejected.

**Undo is a command journal, not snapshots.** `project::Command` records its own
inverse; `UndoStack` applies and reverts. Snapshotting the project on every edit is
simpler but scales with project size, which is unaffordable on this hardware. The
invariant every command must satisfy — `apply` then `revert` restores the exact prior
state — is what the Phase 3 test suite will assert command by command.

The stack is **bounded** at 64 entries. This is a device, not a workstation with
swap. The user-facing promise is "you can always take back the last several things",
which is what a hardware recorder needs.

---

## 5. The `EngineScene` boundary

This is the most important boundary in the system.

The audio engine never sees `project::Project`. It receives an `audio::EngineScene` —
a plain value containing track paths, gains, pans, and the timeline length. When the
project changes, `ProjectManager::buildScene()` produces a new one and the engine
swaps it in at a block boundary.

Why this matters:

- The engine holds no pointer into project state, so an edit on the main loop cannot
  pull data out from under a running render.
- `aw_audio` does not link `aw_project`, so the entire audio path is unit-testable
  with no project code in the binary.
- The port keeps the engine **byte-for-byte identical** — it depends only on `core`,
  `hal` and `events`.

### Phase 4 realtime ownership

`setScene()` performs all path lookup, WAV opening, allocation and initial buffering on
the control thread. It publishes one prepared scene pointer. The audio callback adopts
that pointer only at the next block boundary and sends the previous scene through a
wait-free retirement queue. The streaming thread is the sole reclaimer, so neither
destruction nor allocator work can occur in the callback.

Each clip owns a bounded SPSC queue of 512-frame stereo blocks. Fifteen usable blocks
provide about 160 ms of read-ahead at 48 kHz while keeping memory fixed regardless of
recording length. The streaming thread is the only producer and the audio callback the
only consumer. A generation number invalidates queued blocks after an explicit seek;
loop heads are queued in playback order so ordinary wrapping stays sample-continuous.

Alternatives considered:

- Loading a whole clip is simple but makes memory proportional to recording duration
  and cannot fit the ESP32-P4 target.
- A mutex-protected shared cache simplifies ownership but can priority-invert the audio
  callback behind SD-card I/O.
- Reference-counted scene snapshots simplify lifetime code, but releasing the last
  reference can invoke destruction on the audio thread. Explicit retirement keeps the
  realtime cost fixed and visible.

Live gain, pan, mute, solo and master-gain changes cross a second SPSC command queue and
are applied at block boundaries. They do not rebuild scenes and cannot race scene
replacement. Loop blocks are split at the exact boundary before the transport wraps,
so a hardware period may contain the tail and head of a loop without losing sync.
Meter and position events are throttled to 30 Hz; transport changes remain immediate.

### Record behavior

Bridge uses Portastudio semantics: `Record(track)` arms a track without moving the
playhead; a subsequent `Play` enters `Recording`. `Stop` ends the pass but leaves the
track armed, while explicit disarm returns future Play presses to normal playback.
Phase 4 implements only this transport contract. Capture and punch-in are intentionally
deferred until an audio-input backend exists.

### Raspberry Pi ALSA output

The Pi backend opens one playback PCM as interleaved `S24_3LE`, exactly 48 kHz and
stereo, with ALSA resampling disabled. The six synchronized stereo tracks are already
mixed into that stereo bus by `AudioEngine`; ALSA never sees project tracks and does no
mixing. The requested period is 256 frames and the hardware buffer targets two periods.

A dedicated writer thread attempts `SCHED_FIFO`, pulls one float period from the engine,
packs it to signed 24-bit little-endian without allocation, and handles partial writes.
ALSA's recovery helper repairs xruns and suspended/interrupted writes while the callback
receives an xrun notification for measurement. Immediate drop was chosen for Stop because
monitor audio should not continue draining after the user stops transport.

The composition root wraps ALSA with `FallbackAudioOutput`. Failure to open the selected
PCM logs a warning and activates `NullAudioOutput`, preserving wall-clock transport and
UI behavior with no sound device. Allowing ALSA's `plug` layer to resample was rejected:
it hides incompatible hardware and breaks the fixed-rate timing guarantee.

---

## 6. Display: framebuffer + damage rectangles

Screens draw into an RGB565 `display::Framebuffer` (150 KiB at 320×240). The
framebuffer tracks a union-of-changes damage rectangle. `DisplayFlusher` sends only
that region through `DisplayInterface::blit`.

**Why pixels, not drawing commands, at the HAL boundary.** The panel is on SPI. A
command-per-primitive interface makes every UI element a bus transaction; a rectangle
blit is one DMA transfer. At ~40 MHz, a full 320×240 frame is roughly 30 ms — far too
slow for a moving meter. Damage tracking turns that into a couple of milliseconds for
the parts that actually moved.

**Why RGB565.** Half the memory of RGB888, and it is the native format of every
ST7789/ILI9341-class panel, so nothing converts on the way out. The cost is banding
on gradients, which a UI of flat rectangles and text does not have.

**Why immediate mode, not a retained widget tree.** Screens redraw themselves from
the model whenever they are dirty. There is no tree to keep in sync, and therefore no
"why is this widget stale" class of bug. Redrawing flat rectangles costs well under a
millisecond.

**Why not LVGL.** It solves a problem we do not have (rich, pointer-driven widgets),
costs flash and RAM better spent on audio buffers, encourages touch idioms on a
device with no touchscreen, and would have to be replaced on the ESP32 anyway.

**Why a bitmap font.** No rasteriser, no hinting, no floating point, no allocation,
and glyphs land on exact pixels — which is what makes small text readable on a 320×240
panel. Two sizes cover the whole UI.

---

### The prototype's panel is an HDMI monitor, and its touchscreen is not a pointer

The bench prototype uses a 3.5" 480×320 HDMI LCD with an XPT2046 resistive touch
panel. Two consequences, both of which had to be decided rather than drifted into:

**The display backend is `/dev/fb0`, not an SPI driver.** From the Pi's point of view
that panel is an ordinary monitor. `FramebufferDisplay` reports the *logical* 320×240
canvas through `info()` and maps blits into the physical panel itself, so everything
above draws for the shipping device's screen no matter what is plugged in. The canvas
is letterboxed, not stretched: designing layouts against 480×320 and shrinking them
for the ESP32-P4's 320×240 later is pure rework.

**Touch is a `ButtonInterface`, not a pointer device.** The brief is explicit that the
product has no touchscreen, and that constraint has to survive the prototype or the
interaction design quietly becomes touch-shaped and then fails to port to a device
with a knob and eight switches. So `EvdevTouch` hit-tests a tap against a
`VirtualPanelLayout` — a strip of virtual keys drawn in the space around the canvas —
and emits an ordinary `ButtonId`. Taps on the canvas itself are discarded. Nothing
upstream can tell a virtual key from a real switch, which is exactly the point.

Reading evdev rather than driving the XPT2046 over SPI directly: the kernel driver
already does ADC sampling, pressure thresholding, debouncing and calibration. Writing
that again would be worse in every respect.

**`CompositeButtons`** merges several button backends into one stream, because the
prototype genuinely needs it: the KY-040's push switch is a real GPIO line while the
other controls are virtual keys, and the application is entitled to assume there is
exactly one set of buttons. Composition rather than a backend that knows about other
backends — each source stays a plain `ButtonInterface`, and the merge is testable with
fakes.

None of this is in the shipping device. It lives in `aw_platform_dev`, a separate
library from the Pi assembly, so the distinction is visible in the build rather than
only in a comment.

## 7. MTP is not a filesystem

`MTPInterface` is a separate HAL interface rather than a second `StorageInterface`
implementation, because pretending MTP is a filesystem is exactly how these
integrations go wrong:

- There are no paths, only opaque object handles.
- Handles may be renumbered between sessions.
- There is no seek, no partial write, no rename-in-place, no atomic replace.
- Objects transfer whole, and the size must be declared **before** any data moves.

A `StorageInterface` satisfying both a real SD card and this would be a
lowest-common-denominator API serving neither.

**Product rules encoded in the design, not left to the UI:**

- Nothing is imported automatically. A device appearing produces a notification, not
  a transfer. Auto-sync on a device holding someone's only copy of a take is how you
  lose takes.
- `MtpManager` detects a device but does not open a session until the user asks to
  browse.
- `writeObject` never overwrites — it fails with `AlreadyExists`. Replacing is an
  explicit delete-then-write, after a confirmation screen.
- Import streams to a temp file and renames on success. A cancelled transfer leaves
  nothing behind.
- The TP-7 is never a live playback source. Editing and playback always use local
  copies.

**Why libmtp on the Pi.** MTP has enough device-specific quirks that a mature
implementation is worth the dependency on a platform that can afford it. The ESP32-P4
cannot, which is exactly why the interface above it is only eight operations wide.

---

## 8. Error handling: `Result<T>`, not exceptions

Every fallible operation in the portable layers returns `core::Result<T>` (a
hand-rolled `std::expected` subset) and is `[[nodiscard]]`.

- **Why not exceptions:** the same code must compile for a target where exceptions
  cost flash we do not have, and the audio thread cannot afford unwinding.
- **Why hand-rolled:** `std::expected` is C++23; the project targets C++20 across two
  toolchains. The API is a deliberate subset so it can be aliased to the standard
  type later.
- Exceptions remain legal inside platform backends that already pay for them
  (`std::filesystem`, libmtp wrappers) and are converted to `Error` at the module
  boundary — see `PosixStorage`.
- The realtime path never constructs a `Result` (the message allocates). It reports
  through atomics and the event queue.

---

## 9. The one deliberate global

`core::log` is a process-wide facade over an installable `LogSink`. Threading an
`ILogger&` through every constructor triples the constructor surface of small classes
and makes logging awkward enough that it stops happening. The global holds only a
sink pointer, is installed once by the composition root, and is never read for
control flow — so it does not create the hidden coupling that makes singletons a
problem. It is documented as an exception because it *is* one.

Everything else receives its collaborators by reference from `app::Application`,
which is the only class that knows every other class exists.

---

## 10. UI: one knob, one button, no menus

Navigation is a shallow stack with a checked maximum depth of 4. Exceeding it fires
an assert, because a device driven by one encoder cannot afford a menu tree, and
depth creeps in one screen at a time unless something stops it.

Interaction rules:

- Projects contain exactly six synchronized stereo tracks. The prototype's four physical
  track buttons require a shallow bank/focus mapping, finalized with the Phase 7 UI.
- Encoder push cycles what the encoder edits (gain → pan → position) rather than
  opening a submenu.
- Transport, Back, Undo, Save and Import/Export work on **every** screen, handled
  once in `UiController::handleGlobalShortcut`.
- Screens return a `NavigationRequest` describing intent; the `Navigator` decides.
  Screens never navigate themselves — which is what makes them testable with no
  navigator present (see `tests/ui/UiTests.cpp`).

---

## 11. Testing

`tests/` links only portable modules — no ALSA, no GPIO, no display driver, no
platform backend at all. If a business-logic module ever grows a hardware dependency,
the test target stops linking. That is the architectural claim, mechanically checked.

The fakes that make it work:

- `awtest::MemoryStorage` — an in-RAM `StorageInterface`, including error injection
  ("the card is full", "the file is truncated"), with no filesystem and no cleanup.
- `FakeUsb` / `FakeMtp` in `tests/mtp/` — a USB bus you plug devices into by hand.
- `SpyScreen` in `tests/ui/` — the navigator tested without constructing real screens.

Platform backend logic (key mapping, virtual panel geometry, source merging) is
covered by a **second** executable, `aw_platform_tests`. Folding it into `aw_tests`
would have destroyed the property in the paragraph above, which is the only
mechanical check that the portable layers stay portable.

**Why a hand-rolled 150-line test framework** instead of GoogleTest or Catch2: both
would be fetched at configure time, which means the build stops working on a machine
without network — including, quite possibly, the Pi on the bench. The features used
are "register a test, check a condition, report failures". Swapping in Catch2 later is
a mechanical change to three macros.

---

## 12. Fixed decisions (and why they are not settings)

| Decision | Reason |
| --- | --- |
| 48 kHz only | Removes resampling from the realtime path entirely — the thing that makes multitrack playback feasible on an ESP32-P4. Non-matching files convert once, on import. |
| 24-bit | Matches the TP-7, so exports drop back onto the device with no conversion. |
| 6 synchronized stereo tracks | Fixed product requirement for Bridge: twelve source channels mixed to one stereo monitor/output bus. Physical-button count does not limit project capacity. |
| Float mixing | Keeps the mixer free of format concerns; int conversion is one cheap pass in the backend, which is also where dither will live. |

Options a hardware recorder does not need are options the user has to scroll past.

### WAV streaming and recovery

`WavReader` walks the RIFF chunk list through `hal::FileHandle`, validates every
declared range against both the RIFF boundary and physical file size, and converts
only the caller-requested block into planar float. Unknown chunks are skipped, and
LIST/INFO allocation is capped at 1 MiB so an untrusted import cannot force an
unbounded metadata allocation.

`WavWriter` emits a conventional 44-byte PCM header, appends interleaved blocks, and
patches RIFF/data sizes after each second of newly written audio and again on close.
The alternatives were patching every block (best recovery, but synchronous seeks and
flushes violate the storage contract and amplify flash writes) or only on close (fast,
but a power loss leaves an unusable file). A one-second checkpoint bounds lost tail
audio while keeping filesystem I/O off the realtime callback and out of each block.

RIFF's 32-bit chunk sizes cap a file below 4 GiB. The writer rejects growth beyond
that boundary rather than silently wrapping; RF64 was not selected because TP-7
interoperability matters more than recordings longer than the RIFF limit. Adding RF64
later can be isolated behind the same reader/writer API.

---

## 13. What the ESP32-P4 port will and will not touch

**Reused unchanged:** `core`, `hal` (the interfaces themselves), `events`, `audio`,
`storage`, `project`, `mtp`, `display`, `ui` — everything in the diagram above the
platform layer.

**Rewritten:** `src/platform/` only — I2S instead of ALSA, GPIO ISR instead of
libgpiod, the LCD peripheral instead of spidev, a TinyUSB MTP initiator instead of
libmtp, FatFS instead of `std::filesystem`.

**Known open questions for the port**, flagged now rather than discovered later:

1. `core::Path` is aliased to `std::filesystem::path`. If ESP-IDF's libstdc++ makes
   that too heavy, the alias is the single place to swap it — but business logic must
   keep using `core::Path` and must never call `std::filesystem` free functions.
2. `std::function` is used for MTP transfer callbacks. Fine on the Pi; may want a
   fixed-capacity function type on the P4. Confined to `MTPInterface`.
3. JSON for `metadata.json` — see below.

---

## Resolved phase decisions

- Phase 3 uses a small purpose-built metadata parser: bounded footprint and no supply
  chain, at the cost of supporting only Bridge's known schema.
- Phase 4 uses Portastudio recording semantics: Record arms; Play rolls.
- Phase 5 defaults to 256-frame periods and a two-period hardware buffer. Hardware
  acceptance must measure xruns before this becomes a non-adjustable product constant.
