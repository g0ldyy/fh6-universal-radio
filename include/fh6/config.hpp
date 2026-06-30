#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fh6 {

struct HotkeysConfig {
    int kb_skip = 0;
    int pad_skip = 0;
    int kb_source = 0;
    int pad_source = 0;
    int kb_playpause = 0;
    int pad_playpause = 0;
    int kb_prev = 0;
    int pad_prev = 0;
    int kb_next_station = 0;
    int pad_next_station = 0;
};

struct PlaybackConfig {
    std::string race_start_playback = "next"; // "next" | "restart" | "ignore" | "off"
    bool volume_normalization       = false;
    bool equalizer_enabled          = false;
    std::array<float, 5> equalizer_bands{}; // 60 / 250 / 1000 / 4000 / 12000 Hz, [-6, +6] dB
    bool force_stereo_audio = true;
    // Pre-spawn the next track's pipeline so transitions (skip / end-of-track)
    // are instant.
    bool prebuffer_next_track = true;

    HotkeysConfig hotkeys;
};

struct GeneralConfig {
    uint16_t port               = 8420;
    uint32_t ring_buffer_mb     = 4;
    std::string default_source  = "online_radio";
    std::string fallback_source = "local_files";
    std::filesystem::path ffmpeg_path; // empty = look up on PATH; shared by all sources
    std::filesystem::path yt_dlp_path; // empty = look up on PATH; shared by all sources that require it
};

// A named preset of folders + playback rules.
struct LocalStation {
    std::string name;
    std::vector<std::filesystem::path> roots;    // one or more scan roots
    std::vector<std::filesystem::path> excluded; // absolute folders; folder + descendants skipped
    bool recursive       = true;
    std::string order    = "shuffle"; // "shuffle" | "album" | "name" | "folder"
    std::string grouping = "folder";  // for order=="album": "folder" | "tags"
    std::string repeat   = "all";     // "all" | "one" | "off"
};

struct LocalFilesConfig {
    bool enabled = true;
    std::vector<LocalStation> stations;
    std::string active_station; // station name; empty/unknown => first station
    std::vector<std::string> supported_formats{"mp3", "flac", "wav",  "ogg", "m4a", "opus",
                                               "aac", "wma",  "aiff", "aif", "m3u", "m3u8"};
};

struct YouTubeStation {
    std::string name;
    std::string url;
};

struct YouTubeMusicConfig {
    bool enabled = false;
    std::filesystem::path cookies_path;
    std::vector<YouTubeStation> stations;
    std::string active_station; // station name; empty/unknown => first station
    bool shuffle = true;
};

struct SoundCloudStation {
    std::string name;
    std::string url;
};

struct SoundCloudConfig {
    bool enabled = false;
    std::filesystem::path cookies_path;
    std::vector<SoundCloudStation> stations;
    std::string active_station; // station name; empty/unknown => first station
    bool shuffle = true;
};

struct JellyfinStation {
    std::string name;
    std::string playlist_id;
    bool use_favorites = false;
};

struct JellyfinConfig {
    bool enabled = false;
    std::string server_url;
    std::string api_key;
    std::string user_id;
    std::vector<JellyfinStation> stations;
    std::string active_station;
    bool shuffle = true;
};

struct RadioStation {
    std::string name;
    std::string url;
    std::string favicon; // logo URL (shown as now-playing artwork)
    std::string tags;    // comma-separated genres
    std::string country; // ISO 3166-1 alpha-2 or display name
    std::string codec;
    int bitrate = 0; // kbps
    std::string uuid;       // radio-browser stationuuid, for dedup/click counting
    bool favorite = false;
};

struct OnlineRadioConfig {
    bool enabled = true;
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

struct VanillaRadioConfig {
    bool enabled = false;
};

struct Config {
    GeneralConfig general;
    LocalFilesConfig local_files;
    YouTubeMusicConfig youtube_music;
    SoundCloudConfig soundcloud;
    AudioConfig audio;
    JellyfinConfig jellyfin;
    ExternalAudioConfig external_audio;
    SpotifyConfig spotify;
    OnlineRadioConfig online_radio;
    VanillaRadioConfig vanilla_radio;
    PlaybackConfig playback;
};

// Missing file is fine, defaults are returned.
Config load_config(const std::filesystem::path& path);

// Atomic write (temp + rename). Throws std::system_error on failure.
void save_config(const std::filesystem::path& path, const Config& cfg);

} // namespace fh6
