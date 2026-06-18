export const EQ_BAND_LABELS = ["60 Hz", "250 Hz", "1 kHz", "4 kHz", "12 kHz"];

export const SOURCE_SECTIONS = [
  ["local_files", "Local files"],
  ["youtube_music", "YouTube Music"],
  ["jellyfin", "Jellyfin"],
  ["koel", "Koel / Subsonic"],
  ["external_audio", "External Audio"],
  ["spotify", "Spotify Connect"],
  ["vanilla_radio", "Vanilla Radio"],
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
  [
    "koel",
    "Koel / Subsonic",
    [
      ["enabled", "Enabled", "checkbox"],
      ["server_url", "Server URL (e.g. 'https://koel.example.com')", "text"],
      ["username", "Email", "text"],
      ["password", "API Key", "text"],
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
  [
    "vanilla_radio",
    "Vanilla Radio",
    [
      ["enabled", "Enabled", "checkbox"],
    ],
  ],
  ["audio", "Audio", [["output_gain", "Output gain", "number", 0, 1, 0.01]]],
  [
    "playback",
    "Playback",
    [
      ["race_start_playback", "Race start", "select", ["next", "restart", "ignore"]],
      ["quick_station_skip", "Quick station skip", "checkbox"],
      ["volume_normalization", "Normalize loudness", "checkbox"],
      ["equalizer_enabled", "Equalizer", "checkbox"],
      ["equalizer_bands", "Equalizer bands", "bands"],
      ["force_stereo_audio", "Force stereo audio", "checkbox"],
      ["prebuffer_next_track", "Pre-buffer next track", "checkbox"],
    ],
  ],
];
