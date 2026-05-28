# MinGW-w64 toolchain for cross-compiling the version.dll proxy from Linux.
# Usage:
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
#
# The codebase uses MSVC SEH (__try / __except) for safe memory probing,
# which GCC-mingw doesn't implement. We therefore target Clang-based
# mingw-w64 (llvm-mingw), which accepts the MSVC SEH dialect natively, so
# no portability shim is needed in the source.
#
# The toolchain auto-discovers x86_64-w64-mingw32-clang from PATH or from
# llvm-mingw's standard install prefix. Override with
# -DMINGW_CLANG_CXX=/path/to/x86_64-w64-mingw32-clang++ and/or
# -DMINGW_CLANG_C=/path/to/x86_64-w64-mingw32-clang if your install lives
# elsewhere.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

find_program(MINGW_CLANG_CXX
    NAMES x86_64-w64-mingw32-clang++
    HINTS /opt/llvm-mingw/bin /usr/local/llvm-mingw/bin
)
find_program(MINGW_CLANG_C
    NAMES x86_64-w64-mingw32-clang
    HINTS /opt/llvm-mingw/bin /usr/local/llvm-mingw/bin
)
find_program(MINGW_WINDRES
    NAMES x86_64-w64-mingw32-windres llvm-windres
    HINTS /opt/llvm-mingw/bin /usr/local/llvm-mingw/bin
)

if(NOT MINGW_CLANG_CXX OR NOT MINGW_CLANG_C)
    message(FATAL_ERROR
        "x86_64-w64-mingw32-clang(++) not found. Install llvm-mingw:\n"
        "  Arch     : sudo pacman -S llvm-mingw\n"
        "  Manual   : https://github.com/mstorsjo/llvm-mingw/releases\n"
        "Then add its bin/ to PATH or pass -DMINGW_CLANG_CXX=/path/to/x86_64-w64-mingw32-clang++.")
endif()

set(CMAKE_C_COMPILER   "${MINGW_CLANG_C}")
set(CMAKE_CXX_COMPILER "${MINGW_CLANG_CXX}")
if(MINGW_WINDRES)
    set(CMAKE_RC_COMPILER "${MINGW_WINDRES}")
endif()

# Search libraries/headers under the toolchain's own sysroot (auto-derived
# from the compiler location), but let host tools (cmake, ninja, ...)
# resolve from PATH normally.
get_filename_component(_mingw_bin "${MINGW_CLANG_CXX}" DIRECTORY)
get_filename_component(_mingw_root "${_mingw_bin}" DIRECTORY)
set(CMAKE_FIND_ROOT_PATH "${_mingw_root}/x86_64-w64-mingw32" "${_mingw_root}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
