cmake_minimum_required(VERSION 3.20)
project(fh6_universal_radio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /MP /EHsc /wd4324)
    add_compile_options("$<$<CONFIG:Release>:/O2;/GL;/Gy;/Gw>")
    add_link_options("$<$<CONFIG:Release>:/LTCG;/OPT:REF;/OPT:ICF>")
    add_compile_definitions(_WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX
                            _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
endif()

add_library(version SHARED
    src/proxy/dll_main.cpp
    src/bridge.cpp
    src/log.cpp
    src/config.cpp
    src/config_store.cpp
    src/ring_buffer.cpp
    src/audio_source_manager.cpp
    src/fmod/pe_image.cpp
    src/fmod/sig_scanner.cpp
    src/fmod/radio_discovery.cpp
    src/fmod/dsp_bridge.cpp
    src/fmod/dsp_control_loop.cpp
    src/fmod/metadata_injector.cpp
    src/http/http_server.cpp
    src/sources/airplay_source.cpp
    src/sources/local_file_source.cpp
    src/sources/youtube_music_source.cpp
)

target_include_directories(version PRIVATE
    include
    third_party/cpp-httplib
    third_party/nlohmann
    third_party/toml11
    third_party/miniaudio
)

target_link_libraries(version PRIVATE ws2_32 bcrypt psapi)

set_target_properties(version PROPERTIES
    OUTPUT_NAME version
    PREFIX ""
)
