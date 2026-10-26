#!/usr/bin/env bash
# Run pcapload.exe under Wine (Flatpak build).
#
#   ./run.sh                 -> launcher / game list
#   ./run.sh zuma            -> run Zuma
#   ./run.sh games/peggle.json
#
# One-time setup (as your own user, no root needed):
#   flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
#   flatpak install --user -y flathub org.winehq.Wine/x86_64/stable-26.08
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if ! flatpak info --user org.winehq.Wine >/dev/null 2>&1; then
    echo "Wine (Flatpak) is not installed for user '$(id -un)'." >&2
    echo "Run the two setup commands at the top of this script first." >&2
    exit 1
fi

exec flatpak run --user --command=wine \
    --socket=x11 --socket=pulseaudio \
    --filesystem="$HERE" \
    --env=PCAPLOAD_LOG="${PCAPLOAD_LOG:-0}" \
    org.winehq.Wine "$HERE/pcapload.exe" "$@"
