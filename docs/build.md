# Building and testing

## Requirements

- CMake 3.22+
- A C++20 compiler (GCC 12+ or Clang 15+)
- Ninja (optional, but assumed below)

Phase 1 has **no third-party dependencies**. The Raspberry Pi backend will need
`libasound2-dev`, `libgpiod-dev`, `libusb-1.0-0-dev` and `libmtp-dev` from Phase 5
onwards; the build reports which are missing and keeps using stubs until then.

## Development build (any machine)

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

`AW_PLATFORM` defaults to `simulator` unless `/proc/device-tree/model` says
Raspberry Pi, so this works on a laptop with no hardware attached.

## Running the tests

```bash
./build/tests/aw_tests        # direct, with per-test output
ctest --test-dir build        # through CTest
```

The suite links only portable modules — no ALSA, no GPIO, no display driver. That is
deliberate: see `docs/architecture.md` §11.

## Running the application

```bash
AW_PROJECTS_ROOT=./projects ./build/src/app/audio-workstation
```

On the simulator this starts the full application: event bus, audio engine (driven by
a wall-clock thread that discards its output), an in-memory display, and the UI main
loop at 30 Hz. `Ctrl-C` requests a clean shutdown.

## Raspberry Pi 5 build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAW_PLATFORM=rpi
cmake --build build
```

Selects the `rpi` backend. As of Phase 1 the drivers are stubs that return
`NotImplemented`, so the binary builds and runs but produces no sound and drives no
panel. Phases 5–8 fill them in.

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
