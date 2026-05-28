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
    src/playback_dsp.cpp
    src/ring_buffer.cpp
    src/source_registration.cpp
    src/subprocess.cpp
    src/audio/endpoint_classifier.cpp
    src/audio/miniaudio.cpp
    src/audio/wasapi_loopback_capture.cpp
    src/audio/wasapi_render_devices.cpp
    src/audio_source_manager.cpp
    src/fmod/pe_image.cpp
    src/fmod/sig_scanner.cpp
    src/fmod/radio_discovery.cpp
    src/fmod/dsp_bridge.cpp
    src/fmod/dsp_control_loop.cpp
    src/fmod/game_state_probe.cpp
    src/fmod/metadata_injector.cpp
    src/http/config_json.cpp
    src/http/http_server.cpp
    src/http/http_wire.cpp
    src/http/roon_routes.cpp
    src/roon/roon_control_client.cpp
    src/roon/setup_diagnostics.cpp
    src/roon/roon_sidecar_process.cpp
    src/sources/local_file_source.cpp
    src/sources/roon_source.cpp
    src/sources/youtube_music_support.cpp
    src/sources/youtube_music_source.cpp
)

target_include_directories(version PRIVATE
    include
    third_party/cpp-httplib
    third_party/nlohmann
    third_party/toml11
    third_party/miniaudio
)

target_link_libraries(version PRIVATE ws2_32 bcrypt psapi ole32 uuid propsys)

set_target_properties(version PROPERTIES
    OUTPUT_NAME version
    PREFIX ""
)

add_executable(config_roundtrip_tests
    tests/config_roundtrip_tests.cpp
    src/config.cpp
    src/http/config_json.cpp
    src/log.cpp
)

target_include_directories(config_roundtrip_tests PRIVATE
    include
    third_party/nlohmann
    third_party/toml11
)

add_executable(roon_source_tests
    tests/roon_source_tests.cpp
    src/audio/miniaudio.cpp
    src/audio/wasapi_loopback_capture.cpp
    src/log.cpp
    src/roon/roon_control_client.cpp
    src/roon/roon_sidecar_process.cpp
    src/ring_buffer.cpp
    src/sources/roon_source.cpp
)

target_include_directories(roon_source_tests PRIVATE
    include
    third_party/cpp-httplib
    third_party/miniaudio
    third_party/nlohmann
)

target_link_libraries(roon_source_tests PRIVATE ws2_32 ole32 uuid propsys)

add_executable(source_registration_tests
    tests/source_registration_tests.cpp
    src/audio/miniaudio.cpp
    src/audio/wasapi_loopback_capture.cpp
    src/audio_source_manager.cpp
    src/log.cpp
    src/ring_buffer.cpp
    src/roon/roon_control_client.cpp
    src/roon/roon_sidecar_process.cpp
    src/source_registration.cpp
    src/sources/roon_source.cpp
)

target_include_directories(source_registration_tests PRIVATE
    include
    third_party/cpp-httplib
    third_party/miniaudio
    third_party/nlohmann
)

target_link_libraries(source_registration_tests PRIVATE ws2_32 ole32 uuid propsys)

add_executable(dsp_retarget_policy_tests
    tests/dsp_retarget_policy_tests.cpp
)

target_include_directories(dsp_retarget_policy_tests PRIVATE
    include
)

add_executable(dsp_bridge_install_tests
    tests/dsp_bridge_install_tests.cpp
    src/audio_source_manager.cpp
    src/fmod/dsp_bridge.cpp
    src/fmod/pe_image.cpp
    src/fmod/sig_scanner.cpp
    src/log.cpp
    src/ring_buffer.cpp
)

target_include_directories(dsp_bridge_install_tests PRIVATE
    include
)

add_executable(dsp_render_policy_tests
    tests/dsp_render_policy_tests.cpp
)

target_include_directories(dsp_render_policy_tests PRIVATE
    include
)

add_executable(youtube_music_runtime_tests
    tests/youtube_music_runtime_tests.cpp
    src/log.cpp
    src/playback_dsp.cpp
    src/ring_buffer.cpp
    src/subprocess.cpp
    src/sources/youtube_music_support.cpp
    src/sources/youtube_music_source.cpp
)

target_include_directories(youtube_music_runtime_tests PRIVATE
    include
)

add_executable(roon_control_client_tests
    tests/roon_control_client_tests.cpp
    src/log.cpp
    src/roon/roon_control_client.cpp
)

target_include_directories(roon_control_client_tests PRIVATE
    include
    third_party/cpp-httplib
    third_party/nlohmann
)

target_link_libraries(roon_control_client_tests PRIVATE ws2_32)

add_executable(roon_sidecar_process_tests
    tests/roon_sidecar_process_tests.cpp
    src/log.cpp
    src/roon/roon_sidecar_process.cpp
)

target_include_directories(roon_sidecar_process_tests PRIVATE
    include
)

add_executable(roon_setup_diagnostics_tests
    tests/roon_setup_diagnostics_tests.cpp
    src/audio/endpoint_classifier.cpp
    src/audio/wasapi_render_devices.cpp
    src/roon/setup_diagnostics.cpp
)

target_include_directories(roon_setup_diagnostics_tests PRIVATE
    include
)

target_link_libraries(roon_setup_diagnostics_tests PRIVATE ole32 uuid propsys)

add_executable(wasapi_loopback_capture_tests
    tests/wasapi_loopback_capture_tests.cpp
    src/audio/miniaudio.cpp
    src/audio/wasapi_loopback_capture.cpp
    src/audio/wasapi_render_devices.cpp
    src/log.cpp
    src/ring_buffer.cpp
)

target_include_directories(wasapi_loopback_capture_tests PRIVATE
    include
    third_party/miniaudio
)

target_link_libraries(wasapi_loopback_capture_tests PRIVATE ole32 uuid propsys)

add_executable(audio_endpoint_classifier_tests
    tests/audio_endpoint_classifier_tests.cpp
    src/audio/endpoint_classifier.cpp
)

target_include_directories(audio_endpoint_classifier_tests PRIVATE
    include
)

add_executable(http_server_routes_tests
    tests/http_server_routes_tests.cpp
    src/audio/endpoint_classifier.cpp
    src/audio/miniaudio.cpp
    src/audio/wasapi_loopback_capture.cpp
    src/audio/wasapi_render_devices.cpp
    src/audio_source_manager.cpp
    src/config.cpp
    src/config_store.cpp
    src/fmod/dsp_bridge.cpp
    src/fmod/pe_image.cpp
    src/fmod/sig_scanner.cpp
    src/http/config_json.cpp
    src/http/http_server.cpp
    src/http/http_wire.cpp
    src/http/roon_routes.cpp
    src/playback_dsp.cpp
    src/roon/roon_control_client.cpp
    src/roon/setup_diagnostics.cpp
    src/log.cpp
    src/ring_buffer.cpp
    src/roon/roon_sidecar_process.cpp
    src/subprocess.cpp
    src/sources/local_file_source.cpp
    src/sources/roon_source.cpp
    src/sources/youtube_music_support.cpp
    src/sources/youtube_music_source.cpp
)

target_include_directories(http_server_routes_tests PRIVATE
    include
    third_party/cpp-httplib
    third_party/miniaudio
    third_party/nlohmann
    third_party/toml11
)

target_link_libraries(http_server_routes_tests PRIVATE ws2_32 bcrypt psapi ole32 uuid propsys)
