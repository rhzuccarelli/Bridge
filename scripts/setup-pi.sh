#!/usr/bin/env bash
#
# One-time setup on a fresh Raspberry Pi.
#
# Installs the build tools, the libraries later phases need, and the diagnostic
# utilities; adds you to the groups required for the display, touchscreen, GPIO
# and audio; and creates the projects directory.
#
# Run it ON THE PI:
#     ./scripts/setup-pi.sh
#
# Safe to run more than once.
#
set -euo pipefail

say()  { printf '\n\033[1;36m==> %s\033[0m\n' "$1"; }
warn() { printf '\033[1;33m    ! %s\033[0m\n' "$1"; }
ok()   { printf '\033[1;32m    ok %s\033[0m\n' "$1"; }

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "This script runs on the Raspberry Pi, not on your Mac." >&2
    exit 1
fi

say "Checking the board"
if [[ -r /proc/device-tree/model ]]; then
    MODEL="$(tr -d '\0' < /proc/device-tree/model)"
    ok "$MODEL"
    case "$MODEL" in
        *"Raspberry Pi 5"*) ;;
        *) warn "This project targets the Pi 5. Continuing anyway." ;;
    esac
else
    warn "Not a Raspberry Pi? Continuing anyway."
fi

say "Updating the package list (this takes a minute on first run)"
sudo apt-get update -qq

say "Installing build tools"
sudo apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    git \
    pkg-config
ok "compiler, cmake, ninja, git"

say "Installing libraries for later phases"
# Not needed to build today - every driver is still a stub - but installing them
# now means Phases 5-8 need no second setup pass.
sudo apt-get install -y --no-install-recommends \
    libasound2-dev \
    libgpiod-dev \
    libusb-1.0-0-dev \
    libmtp-dev
ok "alsa, gpiod, libusb, libmtp"

say "Installing the TP-7 MTP permission rule"
sudo install -m 0644 scripts/99-audio-workstation-tp7.rules \
    /etc/udev/rules.d/99-audio-workstation-tp7.rules
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=usb
ok "TP-7 MTP access for the plugdev group"

say "Installing diagnostic tools"
sudo apt-get install -y --no-install-recommends \
    gpiod \
    evtest \
    fbset \
    alsa-utils
ok "gpiodetect, evtest, fbset, aplay"

say "Adding $USER to the hardware groups"
NEEDS_LOGOUT=0
for group in video input gpio audio spi; do
    if ! getent group "$group" >/dev/null; then
        continue          # Not every group exists on every image.
    fi
    if id -nG "$USER" | tr ' ' '\n' | grep -qx "$group"; then
        ok "already in '$group'"
    else
        sudo usermod -aG "$group" "$USER"
        ok "added to '$group'"
        NEEDS_LOGOUT=1
    fi
done

say "Creating the projects directory"
PROJECTS_ROOT="${AW_PROJECTS_ROOT:-$HOME/audio-projects}"
mkdir -p "$PROJECTS_ROOT"
ok "$PROJECTS_ROOT"

say "Enabling the SPI bus (for the OLED display)"
if command -v raspi-config >/dev/null; then
    if [[ -e /dev/spidev0.0 ]]; then
        ok "SPI already enabled (/dev/spidev0.0)"
    else
        sudo raspi-config nonint do_spi 0 && ok "SPI enabled (reboot to take effect)"
        NEEDS_LOGOUT=1
    fi
else
    warn "raspi-config not found - enable SPI by adding 'dtparam=spi=on' to config.txt"
fi

say "Hardware check"
if [[ -e /dev/spidev0.0 ]]; then
    ok "SPI present: /dev/spidev0.0 (OLED can be used)"
else
    warn "no /dev/spidev0.0 yet - reboot after this script for the OLED"
fi

if [[ -e /dev/fb0 ]]; then
    ok "framebuffer present: $(fbset -s 2>/dev/null | awk '/geometry/ {print $2"x"$3", "$6" bpp"}' || echo '/dev/fb0')"
else
    warn "no /dev/fb0 - only relevant if you use the HDMI LCD instead of the OLED"
fi

if ls /dev/input/event* >/dev/null 2>&1; then
    ok "input devices: $(ls /dev/input/event* | tr '\n' ' ')"
    warn "run 'sudo evtest' to confirm which one is the touchscreen"
else
    warn "no /dev/input/event* - touch overlay not enabled yet, see docs/hardware.md"
fi

if command -v gpiodetect >/dev/null; then
    say "GPIO chips (note which one has 54 lines - that is the 40-pin header)"
    gpiodetect || true
    warn "if it is not 'gpiochip0', run the app with AW_GPIO_CHIP=gpiochipN"
fi

say "Done"
if [[ "$NEEDS_LOGOUT" == "1" ]]; then
    warn "You were added to new groups. LOG OUT AND BACK IN before running the app:"
    warn "    exit          # then ssh back in"
fi
echo
echo "Next:  ./scripts/build-pi.sh"
echo
