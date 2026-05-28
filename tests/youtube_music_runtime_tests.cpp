#include "fh6/sources/youtube_music_source.hpp"

#include <windows.h>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error{message};
}

void require_contains(const std::string& text, const char* needle, const char* message) {
    if (text.find(needle) == std::string::npos) throw std::runtime_error{message};
}

std::filesystem::path current_executable_path() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) throw std::runtime_error{"cannot resolve test executable"};
    return std::filesystem::path{buffer};
}

} // namespace

int main() {
    fh6::YouTubeMusicConfig missing_tools;
    missing_tools.enabled = true;
    missing_tools.yt_dlp_path = R"(Z:\fh6-missing-tools\yt-dlp.exe)";
    missing_tools.ffmpeg_path = R"(Z:\fh6-missing-tools\ffmpeg.exe)";

    fh6::sources::YouTubeMusicSource missing_source{missing_tools};
    require(missing_source.initialize(), "enabled YouTube source should stay registered for diagnostics");
    require(missing_source.auth_state() == fh6::AuthState::error,
            "missing configured tools should surface as an auth/setup error");

    const auto instructions = missing_source.auth_instructions();
    require_contains(instructions, "winget install yt-dlp.yt-dlp",
                     "instructions should include the yt-dlp winget command");
    require_contains(instructions, "winget install Gyan.FFmpeg",
                     "instructions should include the ffmpeg winget command");
    require_contains(instructions, "winget install DenoLand.Deno",
                     "instructions should mention Deno for current YouTube playback requirements");
    require_contains(instructions, "[youtube_music].yt_dlp_path",
                     "instructions should name the yt-dlp config field");
    require_contains(instructions, "ffmpeg_path", "instructions should name the ffmpeg config field");

    fh6::YouTubeMusicConfig configured_tools;
    configured_tools.enabled = true;
    configured_tools.yt_dlp_path = current_executable_path();
    configured_tools.ffmpeg_path = current_executable_path();

    fh6::sources::YouTubeMusicSource configured_source{configured_tools};
    require(configured_source.initialize(), "configured tool paths should allow initialization");
    require(configured_source.auth_state() == fh6::AuthState::none_required,
            "configured tool paths without cookies should not require auth");

    return 0;
}
