# Assets

Resources compiled into the binary. Nothing here is read from disk at runtime — the
device must boot and draw its first frame without touching the card, and on the
ESP32-P4 there may be no filesystem available at that point at all.

## fonts/

Phase 7 generates two fixed-pitch bitmap fonts here as C++ arrays:

- `small` — ~6×10, list rows, labels, breadcrumbs.
- `large` — ~12×20, the transport timecode, readable across a room.

Coverage is ASCII 32–126. Rows are top to bottom, MSB is the leftmost pixel; see
`include/aw/display/Font.hpp` for the exact layout the renderer expects.

The generator (a small script producing a `.cpp` from a source font) lands with
Phase 7, along with a checked-in copy of its output so the build never depends on it.
