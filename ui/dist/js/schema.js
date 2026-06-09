export const EQ_BAND_LABELS = ["60 Hz", "250 Hz", "1 kHz", "4 kHz", "12 kHz"];

export const SOURCE_SECTIONS = [
  ["local_files", "Local files"],
  ["youtube_music", "YouTube Music"],
  ["apple_music", "Apple Music"],
  ["jellyfin", "Jellyfin"],
  ["external_audio", "External Audio"],
  ["spotify", "Spotify Connect"],
  ["online_radio", "Online Radio"],
];

// [field, label, type, ...args]. type: checkbox | text | number | select | bands.
export const SCHEMA = [
  [
    "general",
    "General",
    [
      ["port", "Port", "number", 1, 65535],
      ["ring_buffer_mb", "Ring buffer (MB)", "number", 1, 64],
      ["default_source", "Default source", "source-select"],
      ["fallback_source", "Fallback source", "source-select"],
      ["ffmpeg_path", "ffmpeg path (optional)", "text"],
    ],
  ],
  [
    "local_files",
    "Local files",
    [
      ["enabled", "Enabled", "checkbox"],
      // Folders, stations, ordering and the queue live in the dedicated
      // Local Files card on the dashboard (render/localFiles.js).
    ],
  ],
  [
    "youtube_music",
    "YouTube Music",
    [
      ["enabled", "Enabled", "checkbox"],
      ["cookies_path", "cookies.txt (optional)", "text"],
      ["yt_dlp_path", "yt-dlp path (optional)", "text"],
      ["default_playlist", "Default playlist URL", "text"],
      ["shuffle", "Shuffle", "checkbox"],
    ],
  ],
  [
    "apple_music",
    "Apple Music",
    [
      ["enabled", "Enabled", "checkbox"],
      ["transport_controls", "Control playback", "checkbox"],
      ["capture_mode", "Capture mode", "select", ["auto", "process_loopback", "device"]],
      ["capture_device", "Capture device", "text"],
      ["monitor_when_radio_inactive", "Monitor cable outside FH6", "checkbox"],
    ],
  ],
  [
    "jellyfin",
    "Jellyfin",
    [
      ["enabled", "Enabled", "checkbox"],
      ["server_url", "Server URL", "text"],
      ["user_id", "User ID", "text"],
      ["api_key", "API Key", "text"],
      ["default_playlist", "Default Playlist", "text"],
      ["use_favorites", "Use Favorites", "checkbox"],
      ["shuffle", "Shuffle", "checkbox"],
    ],
  ],
  ["external_audio", "External Audio", [["enabled", "Enabled", "checkbox"]]],
  [
    "spotify",
    "Spotify Connect",
    [
      ["enabled", "Enabled", "checkbox"],
      ["librespot_path", "librespot.exe path", "text"],
      ["cache_dir", "Cache directory", "text"],
    ],
  ],
  [
    "online_radio",
    "Online Radio",
    [
      ["enabled", "Enable Online Radio", "checkbox"],
      ["default_station_index", "Default station", "station-select"],
      // Stations, favourites and discovery live in the dedicated Online Radio
      // card on the dashboard (render/onlineRadio.js).
    ],
  ],
  ["audio", "Audio", [["output_gain", "Output gain", "number", 0, 1, 0.01]]],
  [
    "playback",
    "Playback",
    [
      ["race_start_playback", "Race start", "select", ["next", "restart", "ignore"]],
      ["quick_station_skip", "Quick station skip", "checkbox"],
      ["radio_pause_delay_ms", "Radio pause delay (ms)", "number", 20, 5000, 20],
      ["radio_diagnostics", "Radio diagnostics", "checkbox"],
      ["show_album_in_hud", "Show album in HUD", "checkbox"],
      ["volume_normalization", "Normalize loudness", "checkbox"],
      ["equalizer_enabled", "Equalizer", "checkbox"],
      ["equalizer_bands", "Equalizer bands", "bands"],
      ["force_stereo_audio", "Force stereo audio", "checkbox"],
      ["prebuffer_next_track", "Pre-buffer next track", "checkbox"],
    ],
  ],
];
