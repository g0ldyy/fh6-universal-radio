#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic_bool g_running{true};

BOOL WINAPI console_handler(DWORD event)
{
    if (event == CTRL_C_EVENT || event == CTRL_CLOSE_EVENT || event == CTRL_BREAK_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool contains_case_insensitive(std::string haystack, std::string needle)
{
    return lower(std::move(haystack)).find(lower(std::move(needle))) != std::string::npos;
}

bool is_process_running(const wchar_t* exe_name)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, exe_name) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
}

void passthrough_callback(ma_device* device, void* output, const void* input, ma_uint32 frame_count)
{
    const auto channels = device->playback.channels;
    const auto bytes = static_cast<size_t>(frame_count) * channels * sizeof(float);
    if (input) {
        std::memcpy(output, input, bytes);
    } else {
        std::memset(output, 0, bytes);
    }
}

bool find_capture_device(ma_context& context, const std::string& name, ma_device_id& id, std::string& actual_name)
{
    ma_device_info* playback_infos = nullptr;
    ma_uint32 playback_count = 0;
    ma_device_info* capture_infos = nullptr;
    ma_uint32 capture_count = 0;

    if (ma_context_get_devices(&context, &playback_infos, &playback_count, &capture_infos, &capture_count) != MA_SUCCESS) {
        return false;
    }

    for (ma_uint32 i = 0; i < capture_count; ++i) {
        if (contains_case_insensitive(capture_infos[i].name, name)) {
            id = capture_infos[i].id;
            actual_name = capture_infos[i].name;
            return true;
        }
    }

    return false;
}

} // namespace

int main(int argc, char** argv)
{
    SetConsoleCtrlHandler(console_handler, TRUE);

    HANDLE single_instance = CreateMutexW(nullptr, TRUE, L"Local\\FH6UniversalRadioCompanion");
    if (!single_instance || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (single_instance) CloseHandle(single_instance);
        return 0;
    }

    const std::string cable_name = argc > 1 ? argv[1] : "CABLE Output";
    const wchar_t* game_process = L"forzahorizon6.exe";

    ma_context context{};
    if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS) {
        OutputDebugStringW(L"FH6 Radio Companion: failed to initialize audio context.\n");
        CloseHandle(single_instance);
        return 1;
    }

    ma_device_id cable_id{};
    std::string actual_cable_name;
    if (!find_capture_device(context, cable_name, cable_id, actual_cable_name)) {
        OutputDebugStringW(L"FH6 Radio Companion: cable capture device not found.\n");
        ma_context_uninit(&context);
        CloseHandle(single_instance);
        return 1;
    }

    ma_device_config config = ma_device_config_init(ma_device_type_duplex);
    config.capture.pDeviceID = &cable_id;
    config.capture.format = ma_format_f32;
    config.capture.channels = 2;
    config.playback.pDeviceID = nullptr; // Current Windows default output.
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate = 48000;
    config.dataCallback = passthrough_callback;

    ma_device device{};
    if (ma_device_init(&context, &config, &device) != MA_SUCCESS) {
        OutputDebugStringW(L"FH6 Radio Companion: failed to open cable monitor device.\n");
        ma_context_uninit(&context);
        CloseHandle(single_instance);
        return 1;
    }

    bool monitoring = false;
    while (g_running) {
        const bool game_running = is_process_running(game_process);
        if (!game_running && !monitoring) {
            if (ma_device_start(&device) == MA_SUCCESS) {
                monitoring = true;
            } else {
                OutputDebugStringW(L"FH6 Radio Companion: failed to start cable monitor.\n");
            }
        } else if (game_running && monitoring) {
            ma_device_stop(&device);
            monitoring = false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (monitoring) ma_device_stop(&device);
    ma_device_uninit(&device);
    ma_context_uninit(&context);
    CloseHandle(single_instance);
    return 0;
}
