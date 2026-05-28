#!/usr/bin/env bash
# Pulls the FH6 radio-station overlay files (RadioInfo XMLs, FMOD banks,
# Anthem.zip UI atlas) out of an existing radio-mod ZIP. They're modified
# copies of game assets, so the repo doesn't ship them.
#
#   $ scripts/fetch-media.sh /path/to/radio-mod.zip [out-dir]
#
# Output lands in dist/media/ by default, ready for install.sh.

set -euo pipefail

src=${1:?Usage: $0 <radio-mod.zip> [out-dir]}
root=$(cd "$(dirname "$0")/.." && pwd)
out=${2:-$root/dist/media}

[ -f "$src" ] || { echo "Source not found: $src" >&2; exit 1; }
command -v unzip >/dev/null || { echo "unzip is required" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

unzip -q -o "$src" -d "$tmp"

# ZIPs can have a top-level media/ folder or the assets at root, so locate
# whichever variant we got.
inner=$(find "$tmp" -type d -name media -print -quit || true)
if [ -z "$inner" ]; then
    radio=$(find "$tmp" -type f -name RadioInfo_EN.xml -print -quit || true)
    [ -n "$radio" ] && inner=$(dirname "$(dirname "$radio")")
fi
[ -n "$inner" ] || { echo "No media files found inside $src" >&2; exit 1; }

# Start from a clean $out so leftover files from a different mod don't
# get mixed in. Guard against an empty or root path before deleting.
case "$out" in
    ''|'/') echo "Refusing to clean unsafe out dir: '$out'" >&2; exit 1 ;;
esac
rm -rf -- "$out"
mkdir -p "$out"

cp -rT "$inner" "$out"

printf '\033[32mMedia overlay extracted to %s\033[0m\n' "$out"
(cd "$out" && find . -type f -printf '  %P  (%s bytes)\n')
