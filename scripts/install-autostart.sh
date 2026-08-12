#!/usr/bin/env bash
# Install and start Bridge as a systemd service on the Raspberry Pi.
set -euo pipefail

if [[ "$(uname -s)" != "Linux" || ! -r /proc/device-tree/model ]]; then
    echo "This installer must run on the Raspberry Pi." >&2
    exit 1
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY="$REPO_ROOT/build/src/app/audio-workstation"
UNIT_SOURCE="$REPO_ROOT/scripts/audio-workstation.service"
UNIT_TARGET="/etc/systemd/system/audio-workstation.service"

if [[ ! -x "$BINARY" ]]; then
    echo "Bridge is not built. Run ./scripts/build-pi.sh first." >&2
    exit 1
fi

SERVICE_USER="${SUDO_USER:-$USER}"
SERVICE_HOME="$(getent passwd "$SERVICE_USER" | cut -d: -f6)"
if [[ -z "$SERVICE_HOME" ]]; then
    echo "Cannot determine the home directory for $SERVICE_USER." >&2
    exit 1
fi

TEMP_UNIT="$(mktemp)"
trap 'rm -f "$TEMP_UNIT"' EXIT
sed -e "s|%USER%|$SERVICE_USER|g" \
    -e "s|%DIR%|$REPO_ROOT|g" \
    -e "s|Environment=HOME=/home/$SERVICE_USER|Environment=HOME=$SERVICE_HOME|" \
    -e "s|Environment=AW_PROJECTS_ROOT=/home/$SERVICE_USER/audio-projects|Environment=AW_PROJECTS_ROOT=$SERVICE_HOME/audio-projects|" \
    "$UNIT_SOURCE" > "$TEMP_UNIT"

mkdir -p "$SERVICE_HOME/audio-projects"
sudo install -m 0644 "$TEMP_UNIT" "$UNIT_TARGET"
sudo systemctl daemon-reload
sudo systemctl enable --now audio-workstation.service

echo
echo "Bridge now starts automatically at boot."
echo "Status:  systemctl status audio-workstation --no-pager"
echo "Logs:    journalctl -u audio-workstation -f"
echo "Stop:    sudo systemctl stop audio-workstation"
echo "Disable: sudo systemctl disable --now audio-workstation"
