#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fh6 {

struct PlaybackConfig {
    std::string race_start_playback = "next";   // "next" | "restart" | "ignore"
    bool quick_station_skip         = false;
    bool volume_normalization       = false;
    bool equalizer_enabled          = false;
    std::array<float, 5> equalizer_bands{}; // 60 / 250 / 1000 / 4000 / 12000 Hz, [-6, +6] dB
    bool force_stereo_audio         = true;
    // Pre-spawn the next track's pipeline so transitions (skip / end-of-track)
    // are instant.
    bool prebuffer_next_track       = true;
};

struct GeneralConfig {
    uint16_t port                       = 8420;
    uint32_t ring_buffer_mb             = 4;
    std::string default_source          = "local_files";
    std::string fallback_source         = "local_files";
    std::filesystem::path ffmpeg_path;  // empty = look up on PATH; shared by all sources
};

struct LocalFilesConfig {
    bool enabled = true;
    std::filesystem::path music_dir;
    bool recursive = true;
    bool shuffle   = true;
    std::vector<std::string> supported_formats{"mp3",  "flac", "wav", "ogg", "m4a",
                                                "opus", "aac",  "wma", "aiff", "aif"};
};

struct YouTubeMusicConfig {
    bool enabled = false;
    std::filesystem::path cookies_path;
    std::filesystem::path yt_dlp_path; // empty = look up on PATH
    std::string default_playlist;
    bool shuffle = true;
};

struct JellyfinConfig {
    bool enabled = false;
    std::string server_url;
    std::string api_key;
    std::string user_id;
    std::string default_playlist;
    bool use_favorites = false;
    bool shuffle = true;
};

struct RadioStation {
    std::string name;
    std::string url;
};

struct OnlineRadioConfig {
    bool enabled = false;
    std::vector<RadioStation> stations;
    size_t default_station_index = 0;
};

struct AudioConfig {
    float output_gain = 1.0f;
};

struct ExternalAudioConfig {
    bool enabled = false;

    // Empty = current Windows default playback device. Otherwise a full WASAPI
    // endpoint id, or a stable user-entered substring used by ExternalAudioSource.
    std::string endpoint_id;

    // Empty = current Windows media session. Otherwise a SourceAppUserModelId
    // returned by GlobalSystemMediaTransportControls. Used for metadata and
    // transport commands; audio capture still comes from endpoint_id.
    std::string media_session_id;
};

struct SpotifyConfig {
    bool enabled = false;
    std::filesystem::path librespot_path; // empty = look up on PATH
    std::filesystem::path cache_dir = "spotify_cache";
};

struct Config {
    GeneralConfig general;
    LocalFilesConfig local_files;
    YouTubeMusicConfig youtube_music;
    AudioConfig audio;
    JellyfinConfig jellyfin;
    ExternalAudioConfig external_audio;
    SpotifyConfig spotify;
    OnlineRadioConfig online_radio;
    PlaybackConfig playback;
};

// Missing file is fine, defaults are returned.
Config load_config(const std::filesystem::path& path);

// Atomic write (temp + rename). Throws std::system_error on failure.
void save_config(const std::filesystem::path& path, const Config& cfg);

} // namespace fh6
