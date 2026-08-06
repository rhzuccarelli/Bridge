#!/usr/bin/env bash
#
# Copy your working tree to the Pi and build it there.
#
# Run it ON YOUR MAC, from the repository root:
#     ./scripts/deploy-from-mac.sh                    # uses $AW_PI or pi@audiopi.local
#     ./scripts/deploy-from-mac.sh rob@audiopi.local  # or say it explicitly
#     ./scripts/deploy-from-mac.sh --run              # deploy, build, then launch
#
# Why sync-and-build-there rather than cross-compile: building ARM binaries on a
# Mac needs a cross toolchain or Docker, and the Pi 5 compiles this project in
# well under a minute.  The complexity buys nothing.
#
set -euo pipefail

say()  { printf '\n\033[1;36m==> %s\033[0m\n' "$1"; }
fail() { printf '\033[1;31mERROR: %s\033[0m\n' "$1" >&2; exit 1; }

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

TARGET="${AW_PI:-pi@audiopi.local}"
REMOTE_DIR="${AW_PI_DIR:-audio-workstation}"
RUN_AFTER=0

for arg in "$@"; do
    case "$arg" in
        --run) RUN_AFTER=1 ;;
        -*)    fail "unknown option: $arg" ;;
        *)     TARGET="$arg" ;;
    esac
done

command -v rsync >/dev/null || fail "rsync not found (it ships with macOS - is your PATH odd?)"

say "Checking we can reach $TARGET"
if ! ssh -o ConnectTimeout=10 -o BatchMode=yes "$TARGET" true 2>/dev/null; then
    fail "cannot ssh to $TARGET without a password.
    Fix it with:
        ssh-keygen -t ed25519          # only if you have no key yet
        ssh-copy-id $TARGET
    Then try again. If the name itself does not resolve, check the hostname you
    set in Raspberry Pi Imager and try: ping ${TARGET#*@}"
fi

say "Syncing source to $TARGET:$REMOTE_DIR"
# Build directories are excluded deliberately: a Mac build tree on the Pi would
# be full of the wrong architecture's object files.
rsync -az --delete \
    --exclude '.git/' \
    --exclude 'build*/' \
    --exclude '.DS_Store' \
    --exclude 'projects/' \
    --info=stats1 \
    ./ "$TARGET:$REMOTE_DIR/"

say "Building on the Pi"
ssh "$TARGET" "cd '$REMOTE_DIR' && chmod +x scripts/*.sh && ./scripts/build-pi.sh"

if [[ "$RUN_AFTER" == "1" ]]; then
    say "Launching on the Pi (Ctrl-C to stop)"
    # -t gives a real terminal, which the keyboard backend needs.
    ssh -t "$TARGET" "cd '$REMOTE_DIR' && ./scripts/run-pi.sh"
else
    say "Done"
    echo "Run it with:"
    echo "    ssh -t $TARGET 'cd $REMOTE_DIR && ./scripts/run-pi.sh'"
fi
