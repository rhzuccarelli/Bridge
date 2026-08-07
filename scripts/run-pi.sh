#!/usr/bin/env bash
#
# Launch the workstation with sensible settings for the prototype.
#
# Run it ON THE PI:
#     ./scripts/run-pi.sh              # auto-detect display + keyboard
#     ./scripts/run-pi.sh --oled       # SPI OLED (SSD1306/SH1106) + encoder
#     ./scripts/run-pi.sh --terminal   # preview in your SSH terminal instead
#     ./scripts/run-pi.sh --headless   # no display at all
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY="$REPO_ROOT/build/src/app/audio-workstation"

if [[ ! -x "$BINARY" ]]; then
    echo "Not built yet. Run ./scripts/build-pi.sh first." >&2
    exit 1
fi

export AW_PROJECTS_ROOT="${AW_PROJECTS_ROOT:-$HOME/audio-projects}"
export AW_ALSA_DEVICE="${AW_ALSA_DEVICE:-hw:0,0}"
mkdir -p "$AW_PROJECTS_ROOT"

case "${1:-}" in
    --terminal) export AW_DISPLAY=terminal ;;
    --headless) export AW_DISPLAY=memory; export AW_INPUT=none ;;
    --oled)     export AW_DISPLAY=oled ;;
    "")         export AW_DISPLAY="${AW_DISPLAY:-auto}" ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
esac

# A friendly warning rather than a hard failure: the app degrades on its own.
if [[ "$AW_DISPLAY" != "memory" && -e /dev/fb0 && ! -w /dev/fb0 ]]; then
    echo "note: /dev/fb0 exists but is not writable - are you in the 'video' group?" >&2
    echo "      check with 'groups'; log out and back in after being added." >&2
fi

echo "projects : $AW_PROJECTS_ROOT"
echo "display  : $AW_DISPLAY"
echo "audio    : $AW_ALSA_DEVICE (48 kHz, packed 24-bit, stereo)"
echo "keys     : space=play s=stop r=rec 1-4=track enter=push arrows=encoder q=quit"
echo

exec "$BINARY"
