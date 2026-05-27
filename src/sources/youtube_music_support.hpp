#pragma once

#include "fh6/sources/youtube_music_source.hpp"

#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace fh6::sources {

namespace youtube_music_detail {

// PCM contract written by ffmpeg: 48000 Hz * 2 ch * 2 bytes.
inline constexpr std::uint64_t kPcmBytesPerSec = 48000ull * 2ull * 2ull;

std::wstring quote(const std::wstring& s);
std::wstring widen(std::string_view s);
HANDLE open_nul(DWORD access);
HANDLE open_stderr_log();
std::filesystem::path stderr_log_path();
std::string narrow(std::wstring_view ws);
std::string describe_launch_failure(const std::wstring& bin, DWORD ec, bool from_config);
HANDLE create_kill_on_close_job();
HANDLE spawn_in_job(HANDLE job, const std::wstring& cmd, HANDLE stdin_h, HANDLE stdout_h,
                    HANDLE stderr_h);
std::string drain_to_eof(HANDLE pipe);
bool is_playlist_url(std::string_view url);
std::string watch_url_for_id(std::string_view id);

} // namespace youtube_music_detail

struct YouTubeMusicSource::Pipe {
    HANDLE job        = nullptr;
    HANDLE proc_yt    = nullptr;
    HANDLE proc_ff    = nullptr;
    HANDLE proc_title = nullptr;
    HANDLE read_pipe  = nullptr;
    HANDLE title_pipe = nullptr;
    std::string title_buf;
    std::uint64_t bytes_written = 0;
    bool ended                  = false;

    ~Pipe();
};

} // namespace fh6::sources
