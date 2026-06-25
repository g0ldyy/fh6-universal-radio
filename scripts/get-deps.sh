#!/usr/bin/env bash
# Downloads the three single-header dependencies into third_party/.
# Run this once after cloning, before configuring CMake.
#
#   $ scripts/get-deps.sh
#
# Re-runs are safe, existing files are overwritten.

set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
tp=$root/third_party

fetch() {
    local url=$1 out=$2 base=${3:-$tp}
    mkdir -p "$(dirname "$base/$out")"
    printf '\033[36m-> %s\033[0m\n' "$out"
    curl -fsSL "$url" -o "$base/$out"
}

fetch https://raw.githubusercontent.com/nlohmann/json/develop/single_include/nlohmann/json.hpp nlohmann/nlohmann/json.hpp
fetch https://raw.githubusercontent.com/ToruNiina/toml11/main/single_include/toml.hpp           toml11/toml.hpp
fetch https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h                    miniaudio/miniaudio.h
fetch https://raw.githubusercontent.com/nothings/stb/master/stb_image.h                    stb/stb_image.h
fetch https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h              stb/stb_image_write.h
fetch https://raw.githubusercontent.com/nothings/stb/master/stb_image_resize2.h            stb/stb_image_resize2.h
fetch https://raw.githubusercontent.com/Rebzzel/kiero/master/kiero.h                       kiero/kiero.h
fetch https://raw.githubusercontent.com/Rebzzel/kiero/master/kiero.cpp                     kiero/kiero.cpp
fetch https://raw.githubusercontent.com/TsudaKageyu/minhook/master/include/MinHook.h       minhook/include/MinHook.h
fetch https://raw.githubusercontent.com/TsudaKageyu/minhook/master/src/buffer.c            minhook/src/buffer.c
fetch https://raw.githubusercontent.com/TsudaKageyu/minhook/master/src/buffer.h            minhook/src/buffer.h
fetch https://raw.githubusercontent.com/TsudaKageyu/minhook/master/src/hook.c              minhook/src/hook.c
fetch https://raw.githubusercontent.com/TsudaKageyu/minhook/master/src/trampoline.c        minhook/src/trampoline.c
fetch https://raw.githubusercontent.com/TsudaKageyu/minhook/master/src/trampoline.h        minhook/src/trampoline.h
fetch https://raw.githubusercontent.com/TsudaKageyu/minhook/master/src/hde/hde32.c         minhook/src/hde/hde32.c
fetch https://raw.githubusercontent.com/TsudaKageyu/minhook/master/src/hde/hde32.h         minhook/src/hde/hde32.h
fetch https://raw.githubusercontent.com/TsudaKageyu/minhook/master/src/hde/hde64.c         minhook/src/hde/hde64.c
fetch https://raw.githubusercontent.com/TsudaKageyu/minhook/master/src/hde/hde64.h         minhook/src/hde/hde64.h
fetch https://raw.githubusercontent.com/TsudaKageyu/minhook/master/src/hde/pstdint.h       minhook/src/hde/pstdint.h
fetch https://raw.githubusercontent.com/TsudaKageyu/minhook/master/src/hde/table32.h       minhook/src/hde/table32.h
fetch https://raw.githubusercontent.com/TsudaKageyu/minhook/master/src/hde/table64.h       minhook/src/hde/table64.h

printf '\033[33mApplying required patches to Kiero...\033[0m\n'

# Patch kiero.h to enable D3D12 and MinHook
sed -i 's/KIERO_INCLUDE_D3D12  0/KIERO_INCLUDE_D3D12  1/g' "$tp/kiero/kiero.h"
sed -i 's/KIERO_USE_MINHOOK    0/KIERO_USE_MINHOOK    1/g' "$tp/kiero/kiero.h"

if ! grep -q "KIERO_INCLUDE_D3D12  1" "$tp/kiero/kiero.h" || ! grep -q "KIERO_USE_MINHOOK    1" "$tp/kiero/kiero.h"; then
    printf '\033[31mError: kiero.h patch failed. Upstream layout may have changed.\033[0m\n' >&2
    exit 1
fi

# Patch kiero.cpp to add stdlib.h, fix MinHook include path, and fix FARPROC strictness
sed -i 's|# include "minhook/include/MinHook.h"|# include "MinHook.h"|g' "$tp/kiero/kiero.cpp"
sed -i 's/::GetProcAddress/(void*)::GetProcAddress/g' "$tp/kiero/kiero.cpp"
sed -i '1i #include <stdlib.h>' "$tp/kiero/kiero.cpp"
sed -i 's/<Windows.h>/<windows.h>/g' "$tp/kiero/kiero.cpp"

if ! grep -q '# include "MinHook.h"' "$tp/kiero/kiero.cpp" || ! grep -q '#include <stdlib.h>' "$tp/kiero/kiero.cpp"; then
    printf '\033[31mError: kiero.cpp patch failed. Upstream layout may have changed.\033[0m\n' >&2
    exit 1
fi

printf '\n\033[32mAll dependencies fetched into %s.\033[0m\n' "$tp"