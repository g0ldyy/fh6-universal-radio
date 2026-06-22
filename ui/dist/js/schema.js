import { t } from "./i18n.js";

export const EQ_BAND_LABELS = ["60 Hz", "250 Hz", "1 kHz", "4 kHz", "12 kHz"];

export const SOURCE_SECTIONS = () => [
    ["local_files", t("source.local_files")],
    ["youtube_music", "YouTube Music"],
    ["jellyfin", "Jellyfin"],
    ["external_audio", t("source.external_audio")],
    ["spotify", "Spotify Connect"],
    ["vanilla_radio", t("source.vanilla_radio")],
];

// hotkeys
const KB_KEYS = () => [
    [0, t("hotkeys.none")],
    [0x21, t("hotkeys.kb.page_up")],
    [0x22, t("hotkeys.kb.page_down")],
    [0x24, t("hotkeys.kb.home")],
    [0x23, t("hotkeys.kb.end")],
    [0x78, "F9"],
    [0x79, "F10"],
    [0x9999, t("hotkeys.double_tap")]
];

const PAD_BUTTONS = () => [
    [0, t("hotkeys.none")],
    [0x0100, t("hotkeys.pad.lb")],
    [0x4000, t("hotkeys.pad.x")],
    [0x2000, t("hotkeys.pad.b")],
    [0x0040, t("hotkeys.pad.ls")],
    [0x4100, t("hotkeys.pad.lb_x")],
    [0x2100, t("hotkeys.pad.lb_b")],
    [0x0140, t("hotkeys.pad.lb_ls")],
    [0x6000, t("hotkeys.pad.x_b")],
    [0x6100, t("hotkeys.pad.lb_x_b")],
    [0x9999, t("hotkeys.double_tap")]
];

// [field, label, type, ...args]. type: checkbox | text | number | select | bands.
export const SCHEMA = () => [
    [
        "general",
        t("schema.general"),
        [
            ["port", t("schema.general.port"), "number", 1, 65535],
            ["ring_buffer_mb", t("schema.general.ring_buffer"), "number", 1, 64],
            ["default_source", t("schema.general.default_source"), "source-select"],
            ["fallback_source", t("schema.general.fallback_source"), "source-select"],
            ["ffmpeg_path", t("schema.general.ffmpeg_path"), "text"],
        ],
    ],
    [
        "local_files",
        t("source.local_files"),
        [
            ["enabled", t("schema.enabled"), "checkbox"],
        ],
    ],
    [
        "youtube_music",
        "YouTube Music",
        [
            ["enabled", t("schema.enabled"), "checkbox"],
            ["cookies_path", t("schema.youtube_music.cookies_path"), "text"],
            ["yt_dlp_path", t("schema.youtube_music.yt_dlp_path"), "text"],
            ["default_playlist", t("schema.youtube_music.default_playlist"), "text"],
        ],
    ],
    [
        "jellyfin",
        "Jellyfin",
        [
            ["enabled", t("schema.enabled"), "checkbox"],
            ["server_url", t("schema.jellyfin.server_url"), "text"],
            ["user_id", t("schema.jellyfin.user_id"), "text"],
            ["api_key", t("schema.jellyfin.api_key"), "text"],
        ],
    ],
    ["external_audio", t("source.external_audio"), [["enabled", t("schema.enabled"), "checkbox"]]],
    [
        "spotify",
        "Spotify Connect",
        [
            ["enabled", t("schema.enabled"), "checkbox"],
            ["librespot_path", t("schema.spotify.librespot_path"), "text"],
            ["cache_dir", t("schema.spotify.cache_dir"), "text"],
        ],
    ],
    [
        "online_radio",
        t("online_radio.title"),
        [
            ["enabled", t("schema.online_radio.enabled"), "checkbox"],
            ["default_station_index", t("schema.online_radio.default_station"), "station-select"],
        ],
    ],
    [
        "vanilla_radio",
        t("source.vanilla_radio"),
        [
            ["enabled", t("schema.enabled"), "checkbox"],
        ],
    ],
    ["audio", t("schema.audio"), [["output_gain", t("schema.audio.output_gain"), "number", 0, 1, 0.01]]],
    [
        "playback",
        t("schema.playback"),
        [
            ["race_start_playback", t("schema.playback.race_start"), "select", ["next", "restart", "ignore", "off"]],
            ["volume_normalization", t("schema.playback.normalize"), "checkbox"],
            ["equalizer_enabled", t("schema.playback.equalizer"), "checkbox"],
            ["equalizer_bands", t("schema.playback.eq_bands"), "bands"],
            ["force_stereo_audio", t("schema.playback.force_stereo"), "checkbox"],
            ["prebuffer_next_track", t("schema.playback.prebuffer"), "checkbox"],
        ],
    ],
    [
      ["kb_playpause", "Play / Pause (Keyboard)", "keybind-kb", KB_KEYS],
      ["pad_playpause", "Play / Pause (Controller)", "keybind-pad", PAD_BUTTONS],
      ["kb_skip", "Next Track (Keyboard)", "keybind-kb", KB_KEYS],
      ["pad_skip", "Next Track (Controller)", "keybind-pad", PAD_BUTTONS],
      ["kb_prev", "Previous Track (Keyboard)", "keybind-kb", KB_KEYS],
      ["pad_prev", "Previous Track (Controller)", "keybind-pad", PAD_BUTTONS],
      ["kb_next_station", "Cycle Station/Playlist (Keyboard)", "keybind-kb", KB_KEYS],
      ["pad_next_station", "Cycle Station/Playlist (Controller)", "keybind-pad", PAD_BUTTONS],
      ["kb_source", "Switch Source (Keyboard)", "keybind-kb", KB_KEYS],
      ["pad_source", "Switch Source (Controller)", "keybind-pad", PAD_BUTTONS],
    ]
];
