#include "fh6/config.hpp"
#include "fh6/log.hpp"

#include <toml.hpp>

#include <fstream>
#include <span>
#include <system_error>

namespace fh6 {

namespace {

template <class T> T pick(const toml::value& tbl, const char* key, T fallback) {
    try {
        if (!tbl.contains(key)) return fallback;
        return toml::find<T>(tbl, key);
    } catch (...) {
        return fallback;
    }
}

std::filesystem::path pick_path(const toml::value& tbl, const char* key) {
    auto s = pick<std::string>(tbl, key, "");
    return s.empty() ? std::filesystem::path{} : std::filesystem::path{s};
}

const toml::value& section(const toml::value& root, const char* key) {
    static const toml::value empty{toml::table{}};
    try {
        if (root.contains(key)) return root.at(key);
    } catch (...) {}
    return empty;
}

} // namespace

Config load_config(const std::filesystem::path& path) {
    Config cfg;
    if (!std::filesystem::exists(path)) {
        log::info("[config] no config.toml at {}; using defaults", path.string());
        return cfg;
    }

    toml::value root;
    try {
        root = toml::parse(path.string());
    } catch (const std::exception& e) {
        log::warn("[config] parse error in {}: {}", path.string(), e.what());
        return cfg;
    }

    const auto& g    = section(root, "general");
    cfg.general.port = static_cast<uint16_t>(pick<int>(g, "port", cfg.general.port));
    cfg.general.ring_buffer_mb =
        static_cast<uint32_t>(pick<int>(g, "ring_buffer_mb", cfg.general.ring_buffer_mb));
    cfg.general.default_source = pick<std::string>(g, "default_source", cfg.general.default_source);
    cfg.general.fallback_source =
        pick<std::string>(g, "fallback_source", cfg.general.fallback_source);
    cfg.general.ffmpeg_path = pick_path(g, "ffmpeg_path");
    cfg.general.yt_dlp_path = pick_path(g, "yt_dlp_path");

    const auto& lf                 = section(root, "local_files");
    cfg.local_files.enabled        = pick<bool>(lf, "enabled", cfg.local_files.enabled);
    cfg.local_files.active_station = pick<std::string>(lf, "active_station", "");
    try {
        if (lf.contains("supported_formats")) {
            auto v = toml::find<std::vector<std::string>>(lf, "supported_formats");
            if (!v.empty()) cfg.local_files.supported_formats = std::move(v);
        }
    } catch (...) {}

    auto read_paths = [](const toml::value& tbl, const char* key) {
        std::vector<std::filesystem::path> out;
        try {
            if (tbl.contains(key)) {
                for (auto& s : toml::find<std::vector<std::string>>(tbl, key))
                    if (!s.empty()) out.emplace_back(s);
            }
        } catch (...) {}
        return out;
    };
    try {
        if (lf.contains("station")) {
            for (const auto& st : toml::find<std::vector<toml::value>>(lf, "station")) {
                LocalStation s;
                s.name      = pick<std::string>(st, "name", "");
                s.roots     = read_paths(st, "roots");
                s.excluded  = read_paths(st, "excluded");
                s.recursive = pick<bool>(st, "recursive", s.recursive);
                s.order     = pick<std::string>(st, "order", s.order);
                s.grouping  = pick<std::string>(st, "grouping", s.grouping);
                s.repeat    = pick<std::string>(st, "repeat", s.repeat);
                if (s.name.empty())
                    s.name = "Station " + std::to_string(cfg.local_files.stations.size() + 1);
                cfg.local_files.stations.push_back(std::move(s));
            }
        }
    } catch (...) {}

    // Migrate the legacy flat layout (music_dir / recursive / shuffle) into a
    // single "My Music" station so existing installs keep working untouched.
    if (cfg.local_files.stations.empty()) {
        auto music_dir = pick_path(lf, "music_dir");
        LocalStation s;
        s.name      = "My Music";
        s.recursive = pick<bool>(lf, "recursive", true);
        s.order     = pick<bool>(lf, "shuffle", true) ? "shuffle" : "name";
        if (!music_dir.empty()) s.roots.push_back(std::move(music_dir));
        cfg.local_files.stations.push_back(std::move(s));
    }
    if (cfg.local_files.active_station.empty())
        cfg.local_files.active_station = cfg.local_files.stations.front().name;

    const auto& ym                     = section(root, "youtube_music");
    cfg.youtube_music.enabled          = pick<bool>(ym, "enabled", cfg.youtube_music.enabled);
    cfg.youtube_music.cookies_path     = pick_path(ym, "cookies_path");
    cfg.youtube_music.active_station   = pick<std::string>(ym, "active_station", "");
    cfg.youtube_music.shuffle          = pick<bool>(ym, "shuffle", cfg.youtube_music.shuffle);

    try {
        if (ym.contains("stations")) {
            for (const auto& st : toml::find<std::vector<toml::value>>(ym, "stations")) {
                YouTubeStation s;
                s.name = pick<std::string>(st, "name", "");
                s.url  = pick<std::string>(st, "url", "");
                cfg.youtube_music.stations.push_back(std::move(s));
            }
        }
    } catch (...) {}

    // fallback to migrate old configs that still use default_playlist
    if (cfg.youtube_music.stations.empty()) {
        auto dp = pick<std::string>(ym, "default_playlist", "");
        if (!dp.empty()) {
            cfg.youtube_music.stations.push_back({"My Playlist", dp});
        }
    }
    if (cfg.youtube_music.active_station.empty() && !cfg.youtube_music.stations.empty()) {
        cfg.youtube_music.active_station = cfg.youtube_music.stations.front().name;
    }
    if (cfg.general.yt_dlp_path.empty()) {
        auto old_ym_path = pick_path(ym, "yt_dlp_path");
        if (!old_ym_path.empty()) {
            cfg.general.yt_dlp_path = old_ym_path;
        }
    }

    const auto& sc = section(root, "soundcloud");
    cfg.soundcloud.enabled = pick<bool>(sc, "enabled", cfg.soundcloud.enabled);
    cfg.soundcloud.cookies_path = pick_path(sc, "cookies_path");
    cfg.soundcloud.active_station = pick<std::string>(sc, "active_station", "");
    cfg.soundcloud.shuffle = pick<bool>(sc, "shuffle", cfg.soundcloud.shuffle);

    try {
        if (sc.contains("stations")) {
            for (const auto& st : toml::find<std::vector<toml::value>>(sc, "stations")) {
                SoundCloudStation s;
                s.name = pick<std::string>(st, "name", "");
                s.url  = pick<std::string>(st, "url", "");
                cfg.soundcloud.stations.push_back(std::move(s));
            }
        }
    } catch (...) {}

    if (cfg.soundcloud.active_station.empty() && !cfg.soundcloud.stations.empty()) {
        cfg.soundcloud.active_station = cfg.soundcloud.stations.front().name;
    }

    const auto& sp             = section(root, "spotify");
    cfg.spotify.enabled        = pick<bool>(sp, "enabled", cfg.spotify.enabled);
    cfg.spotify.librespot_path = pick_path(sp, "librespot_path");
    if (sp.contains("cache_dir")) {
        cfg.spotify.cache_dir = pick_path(sp, "cache_dir");
    }

    const auto& jf          = section(root, "jellyfin");
    cfg.jellyfin.enabled    = pick<bool>(jf, "enabled", cfg.jellyfin.enabled);
    cfg.jellyfin.server_url = pick<std::string>(jf, "server_url", cfg.jellyfin.server_url);
    cfg.jellyfin.api_key    = pick<std::string>(jf, "api_key", cfg.jellyfin.api_key);
    cfg.jellyfin.user_id    = pick<std::string>(jf, "user_id", cfg.jellyfin.user_id);
    cfg.jellyfin.active_station = pick<std::string>(jf, "active_station", cfg.jellyfin.active_station);
    cfg.jellyfin.shuffle    = pick<bool>(jf, "shuffle", cfg.jellyfin.shuffle);

    try {
        if (jf.contains("stations")) {
            for (const auto& st : toml::find<std::vector<toml::value>>(jf, "stations")) {
                JellyfinStation s;
                s.name          = pick<std::string>(st, "name", "");
                s.playlist_id   = pick<std::string>(st, "playlist_id", "");
                s.use_favorites = pick<bool>(st, "use_favorites", false);
                cfg.jellyfin.stations.push_back(std::move(s));
            }
        }
    } catch (...) {}

    // fallback migration for old configs
    if (cfg.jellyfin.stations.empty()) {
        auto dp = pick<std::string>(jf, "default_playlist", "");
        auto uf = pick<bool>(jf, "use_favorites", false);
        if (!dp.empty() || uf) {
            cfg.jellyfin.stations.push_back({"My Playlist", dp, uf});
        }
    }
    if (cfg.jellyfin.active_station.empty() && !cfg.jellyfin.stations.empty()) {
        cfg.jellyfin.active_station = cfg.jellyfin.stations.front().name;
    }

    const auto& or_sec       = section(root, "online_radio");
    cfg.online_radio.enabled = pick<bool>(or_sec, "enabled", cfg.online_radio.enabled);
    const int station_index =
        pick<int>(or_sec, "default_station_index",
                  static_cast<int>(cfg.online_radio.default_station_index));
    cfg.online_radio.default_station_index = station_index < 0 ? 0u : static_cast<size_t>(station_index);
    for (const auto& st : pick<std::vector<toml::value>>(or_sec, "stations", {})) {
        RadioStation rs;
        rs.name     = pick<std::string>(st, "name", "");
        rs.url      = pick<std::string>(st, "url", "");
        rs.favicon  = pick<std::string>(st, "favicon", "");
        rs.tags     = pick<std::string>(st, "tags", "");
        rs.country  = pick<std::string>(st, "country", "");
        rs.codec    = pick<std::string>(st, "codec", "");
        rs.bitrate  = pick<int>(st, "bitrate", 0);
        if (rs.bitrate < 0) rs.bitrate = 0;
        rs.uuid     = pick<std::string>(st, "uuid", "");
        rs.favorite = pick<bool>(st, "favorite", false);
        cfg.online_radio.stations.push_back(std::move(rs));
    }

    const auto& ea             = section(root, "external_audio");
    cfg.external_audio.enabled = pick<bool>(ea, "enabled", cfg.external_audio.enabled);
    cfg.external_audio.endpoint_id =
        pick<std::string>(ea, "endpoint_id", cfg.external_audio.endpoint_id);
    cfg.external_audio.media_session_id =
        pick<std::string>(ea, "media_session_id", cfg.external_audio.media_session_id);


    const auto& vr = section(root, "vanilla_radio");
    cfg.vanilla_radio.enabled = pick<bool>(vr, "enabled", cfg.vanilla_radio.enabled);

    const auto& au = section(root, "audio");
    cfg.audio.output_gain =
        static_cast<float>(pick<double>(au, "output_gain", cfg.audio.output_gain));

    const auto& pb = section(root, "playback");
    const bool legacy_quick_station_skip =
+        pick<bool>(pb, "quick_station_skip", false);
    {
        auto rs = pick<std::string>(pb, "race_start_playback", cfg.playback.race_start_playback);
        if (rs == "next" || rs == "restart" || rs == "ignore" || rs == "off")
            cfg.playback.race_start_playback = std::move(rs);
    }
    cfg.playback.volume_normalization =
        pick<bool>(pb, "volume_normalization", cfg.playback.volume_normalization);
    cfg.playback.equalizer_enabled =
        pick<bool>(pb, "equalizer_enabled", cfg.playback.equalizer_enabled);
    cfg.playback.force_stereo_audio =
        pick<bool>(pb, "force_stereo_audio", cfg.playback.force_stereo_audio);
    cfg.playback.prebuffer_next_track =
        pick<bool>(pb, "prebuffer_next_track", cfg.playback.prebuffer_next_track);
    try {
        if (pb.contains("equalizer_bands")) {
            auto v = toml::find<std::vector<double>>(pb, "equalizer_bands");
            for (std::size_t i = 0; i < cfg.playback.equalizer_bands.size() && i < v.size(); ++i) {
                auto b = static_cast<float>(v[i]);
                if (b < -6.f) b = -6.f;
                if (b > 6.f) b = 6.f;
                cfg.playback.equalizer_bands[i] = b;
            }
        }
    } catch (...) {}

    const auto& hk = section(root, "hotkeys");
    cfg.playback.hotkeys.kb_skip = pick<int>(hk, "kb_skip", cfg.playback.hotkeys.kb_skip);
    cfg.playback.hotkeys.pad_skip = pick<int>(hk, "pad_skip", cfg.playback.hotkeys.pad_skip);
    cfg.playback.hotkeys.kb_source = pick<int>(hk, "kb_source", cfg.playback.hotkeys.kb_source);
    cfg.playback.hotkeys.pad_source = pick<int>(hk, "pad_source", cfg.playback.hotkeys.pad_source);
    cfg.playback.hotkeys.kb_playpause = pick<int>(hk, "kb_playpause", cfg.playback.hotkeys.kb_playpause);
    cfg.playback.hotkeys.pad_playpause = pick<int>(hk, "pad_playpause", cfg.playback.hotkeys.pad_playpause);
    cfg.playback.hotkeys.kb_prev = pick<int>(hk, "kb_prev", cfg.playback.hotkeys.kb_prev);
    cfg.playback.hotkeys.pad_prev = pick<int>(hk, "pad_prev", cfg.playback.hotkeys.pad_prev);
    cfg.playback.hotkeys.kb_next_station = pick<int>(hk, "kb_next_station", cfg.playback.hotkeys.kb_next_station);
    cfg.playback.hotkeys.pad_next_station = pick<int>(hk, "pad_next_station", cfg.playback.hotkeys.pad_next_station);

    const bool any_hotkey_bound =
        cfg.playback.hotkeys.kb_skip || cfg.playback.hotkeys.pad_skip ||
        cfg.playback.hotkeys.kb_source || cfg.playback.hotkeys.pad_source ||
        cfg.playback.hotkeys.kb_playpause || cfg.playback.hotkeys.pad_playpause ||
        cfg.playback.hotkeys.kb_prev || cfg.playback.hotkeys.pad_prev ||
        cfg.playback.hotkeys.kb_next_station || cfg.playback.hotkeys.pad_next_station;
    if (legacy_quick_station_skip && !any_hotkey_bound) {
        cfg.playback.hotkeys.kb_skip = 0x9999; // legacy quick-skip sentinel
        cfg.playback.hotkeys.pad_skip = 0x9999; // legacy quick-skip sentinel
    }

    return cfg;
}

namespace {

// Hand-rolled emitter. toml11's serialiser output changes across majors
// and we want the file to stay diff-friendly for hand edits.
struct Emitter {
    std::string out;

    void header(const char* name) {
        if (!out.empty()) out += '\n';
        out += '[';
        out += name;
        out += "]\n";
    }
    void array_header(const char* name) {
        if (!out.empty()) out += '\n';
        out += "[[";
        out += name;
        out += "]]\n";
    }
    // Literal (single-quoted) strings don't process escapes, which is what we
    // want for Windows paths. Use them when the value contains \ or " and no ';
    // otherwise basic double-quoted with backslash escaping.
    void quoted(std::string_view str) {
        const bool has_bs = str.find('\\') != std::string_view::npos;
        const bool has_dq = str.find('"') != std::string_view::npos;
        const bool has_sq = str.find('\'') != std::string_view::npos;
        if ((has_bs || has_dq) && !has_sq) {
            out += '\'';
            out += str;
            out += '\'';
        } else {
            out += '"';
            for (char c : str) {
                if (c == '\\' || c == '"') out += '\\';
                out += c;
            }
            out += '"';
        }
    }
    void kv(std::string_view key, std::string_view str) {
        out += key;
        out += " = ";
        quoted(str);
        out += '\n';
    }
    void kv_paths(std::string_view key, const std::vector<std::filesystem::path>& v) {
        out += key;
        out += " = [";
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i) out += ", ";
            quoted(v[i].string());
        }
        out += "]\n";
    }
    void kv(std::string_view key, bool v) {
        out += key;
        out += " = ";
        out += v ? "true" : "false";
        out += '\n';
    }
    void kv(std::string_view key, int64_t v) {
        out += key;
        out += " = ";
        out += std::to_string(v);
        out += '\n';
    }
    void kv(std::string_view key, double v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", v);
        out += key;
        out += " = ";
        out += buf;
        out += '\n';
    }
    void kv_path(std::string_view key, const std::filesystem::path& p) {
        kv(key, p.empty() ? std::string{} : p.string());
    }
    void kv_strs(std::string_view key, const std::vector<std::string>& v) {
        out += key;
        out += " = [";
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i) out += ", ";
            out += '"';
            out += v[i];
            out += '"';
        }
        out += "]\n";
    }
    void kv_floats(std::string_view key, std::span<const float> v) {
        out += key;
        out += " = [";
        char buf[32];
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i) out += ", ";
            std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v[i]));
            out += buf;
        }
        out += "]\n";
    }
};

} // namespace

void save_config(const std::filesystem::path& path, const Config& cfg) {
    Emitter e;
    e.header("general");
    e.kv("port", (int64_t)cfg.general.port);
    e.kv("ring_buffer_mb", (int64_t)cfg.general.ring_buffer_mb);
    e.kv("default_source", cfg.general.default_source);
    e.kv("fallback_source", cfg.general.fallback_source);
    e.kv_path("ffmpeg_path", cfg.general.ffmpeg_path);
    e.kv_path("yt_dlp_path", cfg.general.yt_dlp_path);

    e.header("local_files");
    e.kv("enabled", cfg.local_files.enabled);
    e.kv("active_station", cfg.local_files.active_station);
    e.kv_strs("supported_formats", cfg.local_files.supported_formats);
    for (const auto& st : cfg.local_files.stations) {
        e.array_header("local_files.station");
        e.kv("name", st.name);
        e.kv_paths("roots", st.roots);
        e.kv_paths("excluded", st.excluded);
        e.kv("recursive", st.recursive);
        e.kv("order", st.order);
        e.kv("grouping", st.grouping);
        e.kv("repeat", st.repeat);
    }

    e.header("youtube_music");
    e.kv("enabled", cfg.youtube_music.enabled);
    e.kv_path("cookies_path", cfg.youtube_music.cookies_path);

    e.kv("active_station", cfg.youtube_music.active_station);
    e.kv("shuffle", cfg.youtube_music.shuffle);
    for (const auto& st : cfg.youtube_music.stations) {
        e.array_header("youtube_music.stations");
        e.kv("name", st.name);
        e.kv("url", st.url);
    }

    e.header("soundcloud");
    e.kv("enabled", cfg.soundcloud.enabled);
    e.kv_path("cookies_path", cfg.soundcloud.cookies_path);
    e.kv("active_station", cfg.soundcloud.active_station);
    e.kv("shuffle", cfg.soundcloud.shuffle);
    for (const auto& st : cfg.soundcloud.stations) {
        e.array_header("soundcloud.stations");
        e.kv("name", st.name);
        e.kv("url", st.url);
    }

    e.header("jellyfin");
    e.kv("enabled", cfg.jellyfin.enabled);
    e.kv("server_url", cfg.jellyfin.server_url);
    e.kv("api_key", cfg.jellyfin.api_key);
    e.kv("user_id", cfg.jellyfin.user_id);
    e.kv("active_station", cfg.jellyfin.active_station);
    e.kv("shuffle", cfg.jellyfin.shuffle);
    for (const auto& st : cfg.jellyfin.stations) {
        e.array_header("jellyfin.stations");
        e.kv("name", st.name);
        e.kv("playlist_id", st.playlist_id);
        e.kv("use_favorites", st.use_favorites);
    }

    e.header("external_audio");
    e.kv("enabled", cfg.external_audio.enabled);
    e.kv("endpoint_id", cfg.external_audio.endpoint_id);
    e.kv("media_session_id", cfg.external_audio.media_session_id);

    e.header("vanilla_radio");
    e.kv("enabled", cfg.vanilla_radio.enabled);

    e.header("spotify");
    e.kv("enabled", cfg.spotify.enabled);
    e.kv_path("librespot_path", cfg.spotify.librespot_path);
    e.kv_path("cache_dir", cfg.spotify.cache_dir);

    e.header("online_radio");
    e.kv("enabled", cfg.online_radio.enabled);
    e.kv("default_station_index", (int64_t)cfg.online_radio.default_station_index);
    for (const auto& st : cfg.online_radio.stations) {
        e.array_header("online_radio.stations");
        e.kv("name", st.name);
        e.kv("url", st.url);
        if (!st.favicon.empty()) e.kv("favicon", st.favicon);
        if (!st.tags.empty()) e.kv("tags", st.tags);
        if (!st.country.empty()) e.kv("country", st.country);
        if (!st.codec.empty()) e.kv("codec", st.codec);
        if (st.bitrate) e.kv("bitrate", (int64_t)st.bitrate);
        if (!st.uuid.empty()) e.kv("uuid", st.uuid);
        if (st.favorite) e.kv("favorite", true);
    }

    e.header("audio");
    e.kv("output_gain", (double)cfg.audio.output_gain);

    e.header("playback");
    e.kv("race_start_playback", cfg.playback.race_start_playback);
    e.kv("volume_normalization", cfg.playback.volume_normalization);
    e.kv("equalizer_enabled", cfg.playback.equalizer_enabled);
    e.kv_floats("equalizer_bands", std::span<const float>{cfg.playback.equalizer_bands});
    e.kv("force_stereo_audio", cfg.playback.force_stereo_audio);
    e.kv("prebuffer_next_track", cfg.playback.prebuffer_next_track);

    e.header("hotkeys");
    e.kv("kb_skip", (int64_t)cfg.playback.hotkeys.kb_skip);
    e.kv("pad_skip", (int64_t)cfg.playback.hotkeys.pad_skip);
    e.kv("kb_source", (int64_t)cfg.playback.hotkeys.kb_source);
    e.kv("pad_source", (int64_t)cfg.playback.hotkeys.pad_source);
    e.kv("kb_playpause", (int64_t)cfg.playback.hotkeys.kb_playpause);
    e.kv("pad_playpause", (int64_t)cfg.playback.hotkeys.pad_playpause);
    e.kv("kb_prev", (int64_t)cfg.playback.hotkeys.kb_prev);
    e.kv("pad_prev", (int64_t)cfg.playback.hotkeys.pad_prev);
    e.kv("kb_next_station", (int64_t)cfg.playback.hotkeys.kb_next_station);
    e.kv("pad_next_station", (int64_t)cfg.playback.hotkeys.pad_next_station);

    auto tmp  = path;
    tmp      += ".tmp";
    {
        std::ofstream os{tmp, std::ios::binary | std::ios::trunc};
        if (!os) throw std::system_error{errno, std::system_category(), tmp.string()};
        os.write(e.out.data(), (std::streamsize)e.out.size());
        if (!os) throw std::system_error{errno, std::system_category(), tmp.string()};
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) throw std::system_error{ec};
}

} // namespace fh6
