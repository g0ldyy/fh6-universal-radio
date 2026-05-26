# CMake toolchain file for cross-compiling to Windows (x64) using MinGW-w64
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=mingw64-toolchain.cmake ..

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# MinGW-w64 cross compiler prefixes (Fedora paths)
set(MINGW_PREFIX "/usr/x86_64-w64-mingw32")

# Compilers
set(CMAKE_C_COMPILER "/usr/bin/x86_64-w64-mingw32-gcc")
set(CMAKE_CXX_COMPILER "/usr/bin/x86_64-w64-mingw32-g++")
set(CMAKE_RC_COMPILER "/usr/bin/x86_64-w64-mingw32-windres")

# Search paths
set(CMAKE_FIND_ROOT_PATH "${MINGW_PREFIX}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Compiler flags for MinGW
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -std=c++20")

# Windows-specific definitions
add_compile_definitions(_WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
