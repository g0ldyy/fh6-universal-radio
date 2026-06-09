cmake_minimum_required(VERSION 3.20)
project(fh6_universal_radio LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Target is always a Windows PE
if(NOT WIN32)
    message(FATAL_ERROR
        "This project targets Windows. On Linux, configure with a MinGW-w64 "
        "toolchain file, e.g. -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake")
endif()

add_compile_definitions(_WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX
                        UNICODE _UNICODE)

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /MP /EHsc /wd4324)
    add_compile_options("$<$<CONFIG:Release>:/O2;/GL;/Gy;/Gw>")
    add_link_options("$<$<CONFIG:Release>:/LTCG;/OPT:REF;/OPT:ICF>")
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    # llvm-mingw (Clang). Match the MSVC profile as closely as makes sense:
    # high warnings, ThinLTO + dead-section stripping in Release, and a
    # statically linked C++ runtime so the resulting DLL has no libc++ /
    # libunwind / libwinpthread dependency at load time.
    # -fms-extensions enables MSVC SEH keywords (__try / __except) that the
    # codebase uses to guard memory probes around game-owned pointers.
    add_compile_options(-fms-extensions)
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unknown-pragmas
                        -Wno-cast-function-type -Wno-language-extension-token
                        -Wno-deprecated-literal-operator)
    add_compile_options("$<$<CONFIG:Release>:-O3;-flto=thin;-ffunction-sections;-fdata-sections>")
    add_link_options("$<$<CONFIG:Release>:-flto=thin;-Wl,--gc-sections;-s>")
    add_link_options(-static)
endif()

add_library(version SHARED
    src/proxy/dll_main.cpp
    src/bridge.cpp
    src/log.cpp
    src/config.cpp
    src/config_store.cpp
    src/deps.cpp
    src/playback_dsp.cpp
    src/ring_buffer.cpp
    src/subprocess.cpp
    src/net/http_get.cpp
    src/audio_source_manager.cpp
    src/fmod/pe_image.cpp
    src/fmod/sig_scanner.cpp
    src/fmod/radio_discovery.cpp
    src/fmod/dsp_bridge.cpp
    src/fmod/dsp_control_loop.cpp
    src/fmod/game_state_probe.cpp
    src/fmod/metadata_injector.cpp
    src/http/http_server.cpp
    src/sources/local_file_source.cpp
    src/sources/youtube_music_source.cpp
    src/sources/apple_music_source.cpp
    src/sources/jellyfin_source.cpp
    src/sources/external_audio_source.cpp
    src/sources/external_media_session.cpp
    src/sources/spotify_source.cpp
    src/worker/worker_client.cpp
    src/sources/online_radio_source.cpp
)

# MSVC carries the forwarders via pragmas in dll_main.cpp because its .def
# parser rejects absolute paths in forwarder targets; MinGW takes the .def.
if(NOT MSVC)
    target_sources(version PRIVATE src/proxy/version.def)
endif()

set(FH6_CPPWINRT_HINTS)
if(DEFINED ENV{WindowsSdkDir} AND DEFINED ENV{WindowsSDKVersion})
    list(APPEND FH6_CPPWINRT_HINTS
         "$ENV{WindowsSdkDir}/Include/$ENV{WindowsSDKVersion}/cppwinrt")
endif()
if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
    list(APPEND FH6_CPPWINRT_HINTS
         "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/cppwinrt"
         "$ENV{ProgramFiles}/Windows Kits/10/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/cppwinrt")
endif()

find_path(FH6_CPPWINRT_INCLUDE
    NAMES winrt/base.h
    HINTS ${FH6_CPPWINRT_HINTS}
    NO_DEFAULT_PATH
)
if(NOT FH6_CPPWINRT_INCLUDE)
    message(FATAL_ERROR
        "Could not find C++/WinRT headers (winrt/base.h). Install the Windows SDK "
        "or run from a Visual Studio Developer environment that sets WindowsSdkDir "
        "and WindowsSDKVersion.")
endif()

target_include_directories(version PRIVATE
    include
    third_party/nlohmann
    third_party/toml11
    third_party/miniaudio
    "${FH6_CPPWINRT_INCLUDE}"
)

target_link_libraries(version PRIVATE
    ws2_32 bcrypt psapi winhttp ole32 windowsapp winmm uuid ksuser runtimeobject
)

set_target_properties(version PROPERTIES
    OUTPUT_NAME version
    PREFIX ""
)

add_executable(fh6-radio-companion
    src/companion/cable_monitor.cpp
    src/companion/fh6-radio-companion.rc
)

if(MSVC)
    set_target_properties(fh6-radio-companion PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
    )
else()
    target_link_options(fh6-radio-companion PRIVATE -mwindows)
endif()

target_include_directories(fh6-radio-companion PRIVATE
    third_party/miniaudio
)

target_link_libraries(fh6-radio-companion PRIVATE ole32)

# Worker process: lightweight spawner that lives outside the game's address
# space so CreateProcess (= fork on Wine) is cheap.
add_executable(fh6-radio-worker
    src/worker/worker_main.cpp
    src/subprocess.cpp
    src/log.cpp
)

target_include_directories(fh6-radio-worker PRIVATE
    include
    third_party/nlohmann
)

target_link_libraries(fh6-radio-worker PRIVATE advapi32)

set_target_properties(fh6-radio-worker PROPERTIES
    OUTPUT_NAME fh6-radio-worker
    PREFIX ""
)
