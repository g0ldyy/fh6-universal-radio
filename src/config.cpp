#include "fh6/config.hpp"
#include "fh6/log.hpp"

#include <toml.hpp>

#include <fstream>
#include <limits>
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

std::filesystem::path pick_path(const toml::value& tbl, const char* key,
                                const std::filesystem::path& fallback = {}) {
    auto s = pick<std::string>(tbl, key, fallback.empty() ? std::string{} : fallback.string());
    return s.empty() ? std::filesystem::path{} : std::filesystem::path{s};
}

uint32_t pick_u32(const toml::value& tbl, const char* key, uint32_t fallback) {
    try {
        if (!tbl.contains(key)) return fallback;
        const auto v              = toml::find<int64_t>(tbl, key);
        constexpr auto kMaxUint32 = static_cast<int64_t>(std::numeric_limits<uint32_t>::max());
        if (v < 0 || v > kMaxUint32) return fallback;
        return static_cast<uint32_t>(v);
    } catch (...) {
        return fallback;
    }
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

    const auto& lf            = section(root, "local_files");
    cfg.local_files.enabled   = pick<bool>(lf, "enabled", cfg.local_files.enabled);
    cfg.local_files.music_dir = pick_path(lf, "music_dir");
    cfg.local_files.recursive = pick<bool>(lf, "recursive", cfg.local_files.recursive);
    cfg.local_files.shuffle   = pick<bool>(lf, "shuffle", cfg.local_files.shuffle);
    try {
        if (lf.contains("supported_formats")) {
            auto v = toml::find<std::vector<std::string>>(lf, "supported_formats");
            if (!v.empty()) cfg.local_files.supported_formats = std::move(v);
        }
    } catch (...) {}

    const auto& ym                     = section(root, "youtube_music");
    cfg.youtube_music.enabled          = pick<bool>(ym, "enabled", cfg.youtube_music.enabled);
    cfg.youtube_music.cookies_path     = pick_path(ym, "cookies_path");
    cfg.youtube_music.yt_dlp_path      = pick_path(ym, "yt_dlp_path");
    cfg.youtube_music.ffmpeg_path      = pick_path(ym, "ffmpeg_path");
    cfg.youtube_music.default_playlist = pick<std::string>(ym, "default_playlist", "");

    const auto& ro       = section(root, "roon");
    cfg.roon.enabled     = pick<bool>(ro, "enabled", cfg.roon.enabled);
    cfg.roon.node_path   = pick_path(ro, "node_path", cfg.roon.node_path);
    cfg.roon.bridge_path = pick_path(ro, "bridge_path", cfg.roon.bridge_path);
    cfg.roon.selected_core_id =
        pick<std::string>(ro, "selected_core_id", cfg.roon.selected_core_id);
    cfg.roon.selected_zone_id =
        pick<std::string>(ro, "selected_zone_id", cfg.roon.selected_zone_id);
    cfg.roon.selected_output_id =
        pick<std::string>(ro, "selected_output_id", cfg.roon.selected_output_id);
    cfg.roon.capture_device_id =
        pick<std::string>(ro, "capture_device_id", cfg.roon.capture_device_id);
    cfg.roon.capture_device_name =
        pick<std::string>(ro, "capture_device_name", cfg.roon.capture_device_name);
    cfg.roon.control_volume    = pick<bool>(ro, "control_volume", cfg.roon.control_volume);
    cfg.roon.auto_start_bridge = pick<bool>(ro, "auto_start_bridge", cfg.roon.auto_start_bridge);
    cfg.roon.auto_reconnect    = pick<bool>(ro, "auto_reconnect", cfg.roon.auto_reconnect);
    cfg.roon.latency_ms        = pick_u32(ro, "latency_ms", cfg.roon.latency_ms);
    cfg.roon.metadata_poll_ms  = pick_u32(ro, "metadata_poll_ms", cfg.roon.metadata_poll_ms);

    const auto& au = section(root, "audio");
    cfg.audio.output_gain =
        static_cast<float>(pick<double>(au, "output_gain", cfg.audio.output_gain));

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
    void kv(std::string_view key, std::string_view str) {
        // Literal (single-quoted) strings don't process escapes, which is
        // what we want for Windows paths. Use them when the value contains
        // \ or " and no '; otherwise basic double-quoted with backslash
        // escaping.
        const bool has_bs  = str.find('\\') != std::string_view::npos;
        const bool has_dq  = str.find('"') != std::string_view::npos;
        const bool has_sq  = str.find('\'') != std::string_view::npos;
        out               += key;
        out               += " = ";
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
        out += '\n';
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
};

} // namespace

void save_config(const std::filesystem::path& path, const Config& cfg) {
    Emitter e;
    e.header("general");
    e.kv("port", (int64_t)cfg.general.port);
    e.kv("ring_buffer_mb", (int64_t)cfg.general.ring_buffer_mb);
    e.kv("default_source", cfg.general.default_source);
    e.kv("fallback_source", cfg.general.fallback_source);

    e.header("local_files");
    e.kv("enabled", cfg.local_files.enabled);
    e.kv_path("music_dir", cfg.local_files.music_dir);
    e.kv("recursive", cfg.local_files.recursive);
    e.kv("shuffle", cfg.local_files.shuffle);
    e.kv_strs("supported_formats", cfg.local_files.supported_formats);

    e.header("youtube_music");
    e.kv("enabled", cfg.youtube_music.enabled);
    e.kv_path("cookies_path", cfg.youtube_music.cookies_path);
    e.kv_path("yt_dlp_path", cfg.youtube_music.yt_dlp_path);
    e.kv_path("ffmpeg_path", cfg.youtube_music.ffmpeg_path);
    e.kv("default_playlist", cfg.youtube_music.default_playlist);

    e.header("roon");
    e.kv("enabled", cfg.roon.enabled);
    e.kv_path("node_path", cfg.roon.node_path);
    e.kv_path("bridge_path", cfg.roon.bridge_path);
    e.kv("selected_core_id", cfg.roon.selected_core_id);
    e.kv("selected_zone_id", cfg.roon.selected_zone_id);
    e.kv("selected_output_id", cfg.roon.selected_output_id);
    e.kv("capture_device_id", cfg.roon.capture_device_id);
    e.kv("capture_device_name", cfg.roon.capture_device_name);
    e.kv("control_volume", cfg.roon.control_volume);
    e.kv("auto_start_bridge", cfg.roon.auto_start_bridge);
    e.kv("auto_reconnect", cfg.roon.auto_reconnect);
    e.kv("latency_ms", (int64_t)cfg.roon.latency_ms);
    e.kv("metadata_poll_ms", (int64_t)cfg.roon.metadata_poll_ms);

    e.header("audio");
    e.kv("output_gain", (double)cfg.audio.output_gain);

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
