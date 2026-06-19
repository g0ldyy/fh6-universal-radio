export const EQ_BAND_LABELS = ["60 Hz", "250 Hz", "1 kHz", "4 kHz", "12 kHz"];

export const SOURCE_SECTIONS = [
  ["local_files", "Local files"],
  ["youtube_music", "YouTube Music"],
  ["jellyfin", "Jellyfin"],
  ["external_audio", "External Audio"],
  ["spotify", "Spotify Connect"],
  ["vanilla_radio", "Vanilla Radio"],
];

// hotkeys
const KB_KEYS = [
  [0, "None"],
  [0x21, "Page Up"],
  [0x22, "Page Down"],
  [0x24, "Home"],
  [0x23, "End"],
  [0x78, "F9"],
  [0x79, "F10"],
  [0x9999, "Double-Tap Radio Change"]
];

const PAD_BUTTONS = [
  [0, "None"],
  
  // single unbound buttons
  [0x0100, "Left Bumper (LB)"],
  [0x4000, "X Button"],
  [0x2000, "B Button"],
  
  // custom combos (LB / X / B)
  [0x4100, "LB + X"],
  [0x2100, "LB + B"],
  [0x6000, "X + B"],
  [0x6100, "LB + X + B"],

  [0x9999, "Double-Tap Radio Change"]
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
      ["volume_normalization", "Normalize loudness", "checkbox"],
      ["equalizer_enabled", "Equalizer", "checkbox"],
      ["equalizer_bands", "Equalizer bands", "bands"],
      ["force_stereo_audio", "Force stereo audio", "checkbox"],
      ["prebuffer_next_track", "Pre-buffer next track", "checkbox"],
    ],
  ],
  [
    "hotkeys",
    "Global Hotkeys",
    [
      ["kb_playpause", "Play / Pause (Keyboard)", "select-kv", KB_KEYS],
      ["pad_playpause", "Play / Pause (Controller)", "select-kv", PAD_BUTTONS],
      ["kb_skip", "Skip Track (Keyboard)", "select-kv", KB_KEYS],
      ["pad_skip", "Skip Track (Controller)", "select-kv", PAD_BUTTONS],
      ["kb_source", "Switch Source (Keyboard)", "select-kv", KB_KEYS],
      ["pad_source", "Switch Source (Controller)", "select-kv", PAD_BUTTONS],
    ]
  ]
];
