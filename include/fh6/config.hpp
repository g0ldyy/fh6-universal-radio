#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fh6 {

struct GeneralConfig {
    uint16_t port               = 8420;
    uint32_t ring_buffer_mb     = 4;
    std::string default_source  = "local_files";
    std::string fallback_source = "local_files";
};

struct LocalFilesConfig {
    bool enabled = true;
    std::filesystem::path music_dir;
    bool recursive = true;
    bool shuffle   = true;
    std::vector<std::string> supported_formats{"mp3", "flac", "wav", "ogg", "m4a", "opus"};
};

struct YouTubeMusicConfig {
    bool enabled = false;
    std::filesystem::path cookies_path;
    std::filesystem::path yt_dlp_path; // empty = look up on PATH
    std::filesystem::path ffmpeg_path; // empty = look up on PATH
    std::string default_playlist;
};

// Turns the PC into an AirPlay receiver: a phone (e.g. running Apple Music)
// streams audio to it over the LAN and we pump the decoded PCM into the radio.
// Playback is driven entirely from the phone, so this source has no queue.
struct AirPlayConfig {
    bool enabled = false;
    std::filesystem::path receiver_path;    // empty = `shairport-sync` on PATH
    std::filesystem::path ffmpeg_path;       // empty = `ffmpeg` on PATH
    std::string service_name      = "FH6 Radio"; // name shown in the phone's AirPlay picker
    uint32_t input_sample_rate    = 44100;       // PCM rate the receiver emits
};

struct AudioConfig {
    float output_gain = 1.0f;
};

struct Config {
    GeneralConfig general;
    LocalFilesConfig local_files;
    YouTubeMusicConfig youtube_music;
    AirPlayConfig airplay;
    AudioConfig audio;
};

// Missing file is fine, defaults are returned.
Config load_config(const std::filesystem::path& path);

// Atomic write (temp + rename). Throws std::system_error on failure.
void save_config(const std::filesystem::path& path, const Config& cfg);

} // namespace fh6
