# Building and testing

## Requirements

- CMake 3.22+
- A C++20 compiler (GCC 12+ or Clang 15+)
- Ninja (optional, but assumed below)

The portable build has no third-party dependencies. The Raspberry Pi backend requires
`libasound2-dev`, `libgpiod-dev`, `libusb-1.0-0-dev` and `libmtp-dev`.

## Development build (any machine)

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

`AW_PLATFORM` defaults to `simulator` unless `/proc/device-tree/model` says
Raspberry Pi, so this works on a laptop with no hardware attached.

## Running the tests

```bash
./build/tests/aw_tests           # portable modules
./build/tests/aw_platform_tests  # platform backend logic
ctest --test-dir build           # both, through CTest
```

Two executables on purpose. `aw_tests` links only portable modules — no ALSA, no
GPIO, no display driver — and that is the architectural claim this project makes, so
platform tests get their own target rather than diluting it. See
`docs/architecture.md` §11.

## Running the application

```bash
AW_PROJECTS_ROOT=./projects ./build/src/app/audio-workstation
```

This starts the full application: event bus, audio engine (driven by a wall-clock
thread that discards its output), the UI main loop at 30 Hz, and whichever display and
input backends are actually available. `Ctrl-C` requests a clean shutdown.

Display and input are chosen at startup and degrade gracefully — the Linux framebuffer
with a touch key strip on the bench, an ANSI terminal preview over SSH, headless in CI.
`AW_DISPLAY`, `AW_INPUT` and the rest are documented in
[docs/hardware.md](hardware.md), along with the keyboard control map:

```bash
# Live preview of the real framebuffer in your terminal, keyboard driven
AW_DISPLAY=terminal ./build/src/app/audio-workstation
```

## Raspberry Pi 5 build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAW_PLATFORM=rpi
cmake --build build
```

Selects the `rpi` backend and requires `libasound2-dev`. The TP-7 defaults are
`AW_ALSA_DEVICE=hw:TP7,0` and `AW_MIDI_DEVICE=hw:TP7,0,0`; either can be overridden
for bench hardware:

```bash
aplay -l
AW_ALSA_DEVICE=hw:2,0 ./scripts/run-pi.sh --oled
```

Opening Import sends the TP-7's MTP-mode SysEx through the raw-MIDI device. After an
import (or when leaving Import), Bridge releases the libmtp session and the TP-7
automatically re-enumerates as USB audio. Export does not initiate a mode change.

The backend prefers `S24_3LE`, 48 kHz stereo. It also recognizes the TP-7's native
`S24_3LE`, 96 kHz, six-channel playback endpoint: Bridge explicitly upsamples the
48 kHz stereo monitor bus by two, maps it to FL/FR, and silences FC/LFE/RL/RR. This
conversion affects monitoring only; project files and MTP exports stay at 48 kHz.
If neither format can be opened, Bridge logs the error and uses the silent wall-clock
fallback.

Hardware bring-up plays distinct left/right tones for five seconds and fails if an xrun
occurs:

```bash
AW_ALSA_DEVICE=hw:2,0 ./build/src/platform/aw-alsa-test
```

## Strict build

```bash
cmake -S . -B build-strict -G Ninja -DAW_WARNINGS_AS_ERRORS=ON
cmake --build build-strict
```

Warnings are errors. The tree is clean under `-Wall -Wextra -Wpedantic -Wshadow
-Wold-style-cast -Woverloaded-virtual -Wdouble-promotion -Wformat=2`; please keep it
that way.

## Build options

| Option | Default | Meaning |
| --- | --- | --- |
| `AW_PLATFORM` | auto (`simulator`, or `rpi` on a Pi) | Which hardware backend to build. |
| `AW_BUILD_TESTS` | `ON` | Build the unit test suite. |
| `AW_WARNINGS_AS_ERRORS` | `OFF` | Add `-Werror`. |

## Sanitisers

Worth running periodically, particularly for the ring buffer and the display
clipping code:

```bash
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan && ./build-asan/tests/aw_tests
```
