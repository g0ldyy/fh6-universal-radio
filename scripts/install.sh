#!/usr/bin/env bash
# Copies the built mod into a Forza Horizon 6 install. Run after build.sh
# (and fetch-media.sh for the radio-station overlay).
#
#   $ scripts/install.sh /path/to/ForzaHorizon6 [--skip-media]
#
# On Linux the path is typically inside a Proton prefix, e.g.
#   ~/.steam/steam/steamapps/common/ForzaHorizon6
# or, for the Xbox/Game Pass install routed through Proton:
#   ~/.steam/steam/steamapps/compatdata/<appid>/pfx/drive_c/...
#
# Existing files are backed up to *.bak before being overwritten.

set -euo pipefail

game=${1:?Usage: $0 <game-dir> [--skip-media]}
skip_media=${2:-}
root=$(cd "$(dirname "$0")/.." && pwd)
dist=$root/dist
mdir=$dist/media

[ -f "$dist/version.dll" ] || { echo "dist/version.dll not found, run scripts/build.sh first." >&2; exit 1; }
[ -d "$game" ]             || { echo "Game directory not found: $game" >&2; exit 1; }
[ -f "$game/forzahorizon6.exe" ] || {
    printf '\033[33mforzahorizon6.exe not found in %s, make sure this is the right folder.\033[0m\n' "$game" >&2
    exit 1
}

# Copy $1 to $2, backing up any existing target to *.bak.
backup_and_copy() {
    local src=$1 dst=$2
    mkdir -p "$(dirname "$dst")"
    [ -f "$dst" ] && cp -f "$dst" "$dst.bak"
    cp -f "$src" "$dst"
    printf '  + %s\n' "${dst#"$game/"}"
}

backup_and_copy "$dist/version.dll" "$game/version.dll"

mkdir -p "$game/fh6-radio"
cp -rfT "$dist/fh6-radio/ui" "$game/fh6-radio/ui"
if [ ! -f "$game/fh6-radio/config.toml" ]; then
    cp "$dist/fh6-radio/config.toml" "$game/fh6-radio/config.toml"
    printf '\033[33m  + fh6-radio/config.toml  (seeded from example)\033[0m\n'
fi

if [ "$skip_media" != "--skip-media" ]; then
    if [ -d "$mdir" ]; then
        while IFS= read -r -d '' f; do
            backup_and_copy "$f" "$game/media/${f#"$mdir/"}"
        done < <(find "$mdir" -type f -print0)
    else
        printf '\033[33mdist/media/ missing, radio station overlay not installed. Run scripts/fetch-media.sh first.\033[0m\n' >&2
    fi
fi

printf '\n\033[32mDone. Launch the game, set Audio -> Radio DJ = Off, Streamer Mode = On.\033[0m\n'
echo "Then open http://localhost:8420 in any browser on this LAN."
