#pragma once

#include "fh6/sources/youtube_music_source.hpp"

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace fh6::sources {

namespace youtube_music_detail {

inline constexpr std::uint64_t kPcmBytesPerSec = 48000ull * 2ull * 2ull;

std::string drain_to_eof(HANDLE pipe);
bool is_playlist_url(std::string_view url);
std::string watch_url_for_id(std::string_view id);
std::string youtube_tool_setup_message(const YouTubeMusicConfig& cfg);

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
