#include "fh6/http/config_json.hpp"

#include <filesystem>
#include <limits>
#include <string>

namespace fh6::http {

namespace {

using json = nlohmann::json;

std::string path_s(const std::filesystem::path& p) {
    if (p.empty()) return {};
    auto u8 = p.u8string();
    return std::string{u8.begin(), u8.end()};
}

template <class T> T pull(const json& tbl, const char* k, T fallback) {
    if (auto it = tbl.find(k); it != tbl.end() && !it->is_null()) {
        try {
            return it->get<T>();
        } catch (...) {}
    }
    return fallback;
}

uint32_t pull_u32(const json& tbl, const char* k, uint32_t fallback) {
    if (auto it = tbl.find(k); it != tbl.end() && it->is_number_integer()) {
        const auto v              = it->get<int64_t>();
        constexpr auto kMaxUint32 = static_cast<int64_t>(std::numeric_limits<uint32_t>::max());
        if (v >= 0 && v <= kMaxUint32) return static_cast<uint32_t>(v);
    }
    return fallback;
}

std::string pull_string_alias(const json& tbl, const char* canonical, const char* legacy,
                              const std::string& fallback) {
    if (auto it = tbl.find(canonical); it != tbl.end() && !it->is_null()) {
        try {
            return it->get<std::string>();
        } catch (...) {
            return fallback;
        }
    }
    return pull<std::string>(tbl, legacy, fallback);
}

std::filesystem::path pull_path(const json& tbl, const char* k,
                                const std::filesystem::path& fallback) {
    auto s = pull<std::string>(tbl, k, path_s(fallback));
    return s.empty() ? std::filesystem::path{} : std::filesystem::path{s};
}

} // namespace

json config_to_json(const Config& c) {
    return json{
        {"general",
         json{
             {"port", c.general.port},
             {"ring_buffer_mb", c.general.ring_buffer_mb},
             {"default_source", c.general.default_source},
             {"fallback_source", c.general.fallback_source},
         }},
        {"local_files",
         json{
             {"enabled", c.local_files.enabled},
             {"music_dir", path_s(c.local_files.music_dir)},
             {"recursive", c.local_files.recursive},
             {"shuffle", c.local_files.shuffle},
             {"supported_formats", c.local_files.supported_formats},
         }},
        {"youtube_music",
         json{
             {"enabled", c.youtube_music.enabled},
             {"cookies_path", path_s(c.youtube_music.cookies_path)},
             {"yt_dlp_path", path_s(c.youtube_music.yt_dlp_path)},
             {"ffmpeg_path", path_s(c.youtube_music.ffmpeg_path)},
             {"default_playlist", c.youtube_music.default_playlist},
             {"shuffle", c.youtube_music.shuffle},
         }},
        {"roon",
         json{
             {"enabled", c.roon.enabled},
             {"node_path", path_s(c.roon.node_path)},
             {"bridge_path", path_s(c.roon.bridge_path)},
             {"selected_core_id", c.roon.selected_core_id},
             {"selected_zone_id", c.roon.selected_zone_id},
             {"selected_output_id", c.roon.selected_output_id},
             {"render_loopback_endpoint_id", c.roon.render_loopback_endpoint_id},
             {"render_loopback_endpoint_name", c.roon.render_loopback_endpoint_name},
             {"capture_device_id", c.roon.render_loopback_endpoint_id},
             {"capture_device_name", c.roon.render_loopback_endpoint_name},
             {"control_volume", c.roon.control_volume},
             {"auto_start_bridge", c.roon.auto_start_bridge},
             {"auto_reconnect", c.roon.auto_reconnect},
             {"latency_ms", c.roon.latency_ms},
             {"metadata_poll_ms", c.roon.metadata_poll_ms},
         }},
        {"audio",
         json{
             {"output_gain", c.audio.output_gain},
         }},
    };
}

void apply_config_patch(Config& c, const json& j) {
    if (auto it = j.find("general"); it != j.end()) {
        c.general.port            = pull(*it, "port", c.general.port);
        c.general.ring_buffer_mb  = pull(*it, "ring_buffer_mb", c.general.ring_buffer_mb);
        c.general.default_source  = pull(*it, "default_source", c.general.default_source);
        c.general.fallback_source = pull(*it, "fallback_source", c.general.fallback_source);
    }
    if (auto it = j.find("local_files"); it != j.end()) {
        c.local_files.enabled   = pull(*it, "enabled", c.local_files.enabled);
        c.local_files.music_dir = pull_path(*it, "music_dir", c.local_files.music_dir);
        c.local_files.recursive = pull(*it, "recursive", c.local_files.recursive);
        c.local_files.shuffle   = pull(*it, "shuffle", c.local_files.shuffle);
        if (auto fmts = it->find("supported_formats"); fmts != it->end() && fmts->is_array())
            c.local_files.supported_formats = fmts->get<std::vector<std::string>>();
    }
    if (auto it = j.find("youtube_music"); it != j.end()) {
        c.youtube_music.enabled      = pull(*it, "enabled", c.youtube_music.enabled);
        c.youtube_music.cookies_path = pull_path(*it, "cookies_path", c.youtube_music.cookies_path);
        c.youtube_music.yt_dlp_path  = pull_path(*it, "yt_dlp_path", c.youtube_music.yt_dlp_path);
        c.youtube_music.ffmpeg_path  = pull_path(*it, "ffmpeg_path", c.youtube_music.ffmpeg_path);
        c.youtube_music.default_playlist =
            pull(*it, "default_playlist", c.youtube_music.default_playlist);
        c.youtube_music.shuffle = pull(*it, "shuffle", c.youtube_music.shuffle);
    }
    if (auto it = j.find("roon"); it != j.end()) {
        c.roon.enabled            = pull(*it, "enabled", c.roon.enabled);
        c.roon.node_path          = pull_path(*it, "node_path", c.roon.node_path);
        c.roon.bridge_path        = pull_path(*it, "bridge_path", c.roon.bridge_path);
        c.roon.selected_core_id   = pull(*it, "selected_core_id", c.roon.selected_core_id);
        c.roon.selected_zone_id   = pull(*it, "selected_zone_id", c.roon.selected_zone_id);
        c.roon.selected_output_id = pull(*it, "selected_output_id", c.roon.selected_output_id);
        c.roon.render_loopback_endpoint_id =
            pull_string_alias(*it, "render_loopback_endpoint_id", "capture_device_id",
                              c.roon.render_loopback_endpoint_id);
        c.roon.render_loopback_endpoint_name =
            pull_string_alias(*it, "render_loopback_endpoint_name", "capture_device_name",
                              c.roon.render_loopback_endpoint_name);
        c.roon.control_volume    = pull(*it, "control_volume", c.roon.control_volume);
        c.roon.auto_start_bridge = pull(*it, "auto_start_bridge", c.roon.auto_start_bridge);
        c.roon.auto_reconnect    = pull(*it, "auto_reconnect", c.roon.auto_reconnect);
        c.roon.latency_ms        = pull_u32(*it, "latency_ms", c.roon.latency_ms);
        c.roon.metadata_poll_ms  = pull_u32(*it, "metadata_poll_ms", c.roon.metadata_poll_ms);
    }
    if (auto it = j.find("audio"); it != j.end()) {
        c.audio.output_gain = pull(*it, "output_gain", c.audio.output_gain);
    }
}

} // namespace fh6::http
