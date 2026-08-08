# Assets

Resources compiled into the binary. Nothing here is read from disk at runtime — the
device must boot and draw its first frame without touching the card, and on the
ESP32-P4 there may be no filesystem available at that point at all.

## fonts/

Phase 7 generates two fixed-pitch bitmap fonts as C++ arrays:

- `small` — 8×12, list rows, labels, breadcrumbs.
- `large` — 12×16, the transport timecode.

Coverage is ASCII 32–126. Rows are top to bottom, MSB is the leftmost pixel; see
`include/aw/display/Font.hpp` for the exact layout the renderer expects.

`scripts/generate-fonts.py` writes `src/display/GeneratedFonts.inc`. The generated
file is checked in, so neither the Pi build nor the runtime depends on Pillow.
