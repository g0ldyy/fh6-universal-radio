#!/usr/bin/env bash
# Cross-compiles the version.dll proxy from Linux via MinGW-w64, then
# stages everything that needs to ship in dist/.
#
#   $ scripts/build.sh
#
# Output mirrors scripts/build.ps1:
#   dist/version.dll            the proxy DLL (drops next to forzahorizon6.exe)
#   dist/fh6-radio/ui/          dashboard
#   dist/fh6-radio/config.toml  seeded from config.example.toml
#   dist/README.txt             same end-user blurb as the Windows build
#
# Env vars:
#   PATH            must contain x86_64-w64-mingw32-clang++; if your llvm-mingw
#                   lives at /opt/llvm-mingw, this script adds it automatically
#   JOBS            -j parallelism (default: nproc)

set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
build=$root/build
dist=$root/dist

# llvm-mingw is the Clang-based mingw-w64 toolchain. The codebase relies on
# MSVC SEH (__try / __except), which only Clang implements among the free
# cross-compilers. The CMake toolchain file probes the same locations.
for d in /opt/llvm-mingw/bin /usr/local/llvm-mingw/bin; do
    [ -d "$d" ] && PATH=$d:$PATH
done

if ! command -v x86_64-w64-mingw32-clang++ >/dev/null; then
    cat >&2 <<'EOF'
x86_64-w64-mingw32-clang++ not found. Install llvm-mingw:
  Arch     : sudo pacman -S llvm-mingw
  Manual   : https://github.com/mstorsjo/llvm-mingw/releases
EOF
    exit 1
fi

if [ ! -f "$root/third_party/nlohmann/nlohmann/json.hpp" ]; then
    printf '\033[33mthird_party/ is empty, running get-deps.sh first.\033[0m\n'
    "$root/scripts/get-deps.sh"
fi

printf '\033[36m-> cmake configure\033[0m\n'
cmake -S "$root" -B "$build" \
      -DCMAKE_TOOLCHAIN_FILE="$root/cmake/mingw-w64-x86_64.cmake" \
      -DCMAKE_BUILD_TYPE=Release

printf '\033[36m-> cmake build (Release)\033[0m\n'
cmake --build "$build" -j "${JOBS:-$(nproc)}"

# Validate assets are present before copying
if [ ! -f "$root/assets/default_artwork.png" ]; then
    echo "Missing required assets (e.g., default_artwork.png). Please run scripts/get-deps.sh again to fetch missing dependencies." >&2
    exit 1
fi

rm -rf "$dist"
mkdir -p "$dist/fh6-radio"

cp "$build/version.dll"            "$dist/"
cp "$build/fh6-radio-worker.exe"   "$dist/fh6-radio/"
mkdir -p "$dist/fh6-radio/assets"
cp "$root/assets/default_artwork.png" "$dist/fh6-radio/assets/default_artwork.png"
cp -r "$root/ui/dist"              "$dist/fh6-radio/ui"
cp "$root/config.example.toml"     "$dist/fh6-radio/config.toml"

cp "$root/scripts/dist-readme.txt" "$dist/README.txt"

printf '\n\033[32mBuilt + staged in %s\033[0m\n' "$dist"
(cd "$dist" && find . -type f -printf '  %P\n')
