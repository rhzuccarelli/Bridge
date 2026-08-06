#!/usr/bin/env bash
#
# Build and test on the Pi.
#
# Run it ON THE PI, from the repository root:
#     ./scripts/build-pi.sh
#
# Options:
#     ./scripts/build-pi.sh --clean     start from scratch
#     ./scripts/build-pi.sh --run       build, then launch the app
#
set -euo pipefail

say() { printf '\n\033[1;36m==> %s\033[0m\n' "$1"; }

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="build"
RUN_AFTER=0

for arg in "$@"; do
    case "$arg" in
        --clean) rm -rf "$BUILD_DIR" ;;
        --run)   RUN_AFTER=1 ;;
        *) echo "unknown option: $arg" >&2; exit 1 ;;
    esac
done

say "Configuring"
# AW_PLATFORM is detected automatically from /proc/device-tree/model, so on a Pi
# this selects the 'rpi' assembly with no flag needed.
cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo

say "Building"
cmake --build "$BUILD_DIR" --parallel "$(nproc)"

say "Testing"
ctest --test-dir "$BUILD_DIR" --output-on-failure

say "Built: $BUILD_DIR/src/app/audio-workstation"

if [[ "$RUN_AFTER" == "1" ]]; then
    say "Running (Ctrl-C or 'q' to quit)"
    exec ./scripts/run-pi.sh
fi
