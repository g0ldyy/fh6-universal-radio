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
      ["cookies_path", t("schema.yt.cookies_path"), "text"],
      ["yt_dlp_path", t("schema.yt.yt_dlp_path"), "text"],
      ["default_playlist", t("schema.yt.default_playlist"), "text"],
      ["shuffle", t("schema.yt.shuffle"), "checkbox"],
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
      ["default_playlist", t("schema.jellyfin.default_playlist"), "text"],
      ["use_favorites", t("schema.jellyfin.use_favorites"), "checkbox"],
      ["shuffle", t("schema.yt.shuffle"), "checkbox"],
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
      ["race_start_playback", t("schema.playback.race_start"), "select", [
        t("schema.playback.race_start.next"),
        t("schema.playback.race_start.restart"),
        t("schema.playback.race_start.ignore")
      ]],
      ["quick_station_skip", t("schema.playback.quick_skip"), "checkbox"],
      ["volume_normalization", t("schema.playback.normalize"), "checkbox"],
      ["equalizer_enabled", t("schema.playback.equalizer"), "checkbox"],
      ["equalizer_bands", t("schema.playback.eq_bands"), "bands"],
      ["force_stereo_audio", t("schema.playback.force_stereo"), "checkbox"],
      ["prebuffer_next_track", t("schema.playback.prebuffer"), "checkbox"],
    ],
  ],
];
