#include "fh6/http/http_server.hpp"
#include "fh6/audio_source_manager.hpp"
#include "fh6/config_store.hpp"
#include "fh6/deps.hpp"
#include "fh6/fmod/dsp_bridge.hpp"
#include "fh6/log.hpp"
#include "fh6/sources/local_file_source.hpp"
#include "fh6/sources/youtube_music_source.hpp"
#include "fh6/sources/jellyfin_source.hpp"
#include "fh6/sources/external_audio_source.hpp"
#include "fh6/sources/external_media_session.hpp"
#include "fh6/sources/spotify_source.hpp"
#include "fh6/sources/online_radio_source.hpp"

#include <nlohmann/json.hpp>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

namespace fh6::http {

using json = nlohmann::json;

namespace {

constexpr const char* state_string(PlaybackState s) noexcept {
    switch (s) {
        case PlaybackState::stopped: return "stopped";
        case PlaybackState::playing: return "playing";
        case PlaybackState::paused: return "paused";
        case PlaybackState::buffering: return "buffering";
    }
    return "unknown";
}
constexpr const char* auth_string(AuthState s) noexcept {
    switch (s) {
        case AuthState::none_required: return "none_required";
        case AuthState::authenticated: return "authenticated";
        case AuthState::needs_auth: return "needs_auth";
        case AuthState::error: return "error";
    }
    return "unknown";
}
constexpr const char* mode_string(fmod_bridge::DSPMode m) noexcept {
    switch (m) {
        case fmod_bridge::DSPMode::off: return "off";
        case fmod_bridge::DSPMode::passthrough: return "passthrough";
        case fmod_bridge::DSPMode::silence: return "silence";
        case fmod_bridge::DSPMode::pcm: return "pcm";
    }
    return "unknown";
}

// Force UTF-8 so non-ASCII paths (music_dir, cookies_path...) survive the
// JSON round-trip on Windows regardless of the active code page.
std::string path_s(const std::filesystem::path& p) {
    if (p.empty()) return {};
    auto u8 = p.u8string();
    return std::string{u8.begin(), u8.end()};
}

json track_to_json(const TrackInfo& t) {
    return json{
        {"title", t.title},
        {"artist", t.artist},
        {"album", t.album},
        {"artwork_url", t.artwork_url},
        {"duration_ms", t.duration_ms},
        {"position_ms", t.position_ms},
    };
}

json source_to_json(IAudioSource* s) {
    auto c = s->capabilities();
    json j{
        {"name", std::string{s->name()}},
        {"display_name", std::string{s->display_name()}},
        {"playback_state", state_string(s->playback_state())},
        {"auth_state", auth_string(s->auth_state())},
        {"auth_instructions", s->auth_instructions()},
        {"capabilities",
         json{
             {"seek", c.seek},
             {"previous", c.previous},
             {"queue", c.queue},
         }},
        {"details", json::object()},
    };
    if (auto* lf = dynamic_cast<sources::LocalFileSource*>(s)) {
        j["details"]["track_count"]    = lf->track_count();
        j["details"]["station_count"]  = lf->station_count();
        j["details"]["active_station"] = lf->active_station_name();
        j["details"]["order"]          = lf->active_order();
        j["details"]["grouping"]       = lf->active_grouping();
        j["details"]["repeat"]         = lf->active_repeat();
        j["details"]["index_version"]  = lf->index_version();
    }
    if (auto* yt = dynamic_cast<sources::YouTubeMusicSource*>(s)) {
        j["details"]["shuffle"] = yt->shuffle();
        j["details"]["station_count"] = yt->station_count();
        j["details"]["active_station"] = yt->active_station_name();

        auto snap = yt->queue_snapshot();
        j["details"]["queue_size"] = snap.entries.size();
        j["details"]["queue_cursor"] = snap.cursor;
    }
    if (auto* rd = dynamic_cast<sources::OnlineRadioSource*>(s))
        j["details"]["song_history"] = rd->song_history();
    return j;
}

json paths_to_json(const std::vector<std::filesystem::path>& v) {
    json a = json::array();
    for (const auto& p : v) a.push_back(path_s(p));
    return a;
}

json station_to_json(const LocalStation& s) {
    return json{
        {"name", s.name},
        {"roots", paths_to_json(s.roots)},
        {"excluded", paths_to_json(s.excluded)},
        {"recursive", s.recursive},
        {"order", s.order},
        {"grouping", s.grouping},
        {"repeat", s.repeat},
    };
}

json stations_to_json(const std::vector<LocalStation>& v) {
    json a = json::array();
    for (const auto& s : v) a.push_back(station_to_json(s));
    return a;
}

json yt_station_to_json(const YouTubeStation& s) {
    return json{{"name", s.name}, {"url", s.url}};
}

json yt_stations_to_json(const std::vector<YouTubeStation>& v) {
    json a = json::array();
    for (const auto& s : v) a.push_back(yt_station_to_json(s));
    return a;
}

YouTubeStation yt_station_from_json(const json& j) {
    return YouTubeStation{
        j.value("name", ""),
        j.value("url", "")
    };
}

json config_to_json(const Config& c) {
    return json{
        {"general",
         json{
             {"port", c.general.port},
             {"ring_buffer_mb", c.general.ring_buffer_mb},
             {"default_source", c.general.default_source},
             {"fallback_source", c.general.fallback_source},
             {"ffmpeg_path", path_s(c.general.ffmpeg_path)},
         }},
        {"local_files",
         json{
             {"enabled", c.local_files.enabled},
             {"active_station", c.local_files.active_station},
             {"stations", stations_to_json(c.local_files.stations)},
             {"supported_formats", c.local_files.supported_formats},
         }},
        {"youtube_music",
         json{
             {"enabled", c.youtube_music.enabled},
             {"cookies_path", path_s(c.youtube_music.cookies_path)},
             {"yt_dlp_path", path_s(c.youtube_music.yt_dlp_path)},
             {"active_station", c.youtube_music.active_station},
             {"stations", yt_stations_to_json(c.youtube_music.stations)},
             {"shuffle", c.youtube_music.shuffle},
         }},
        {"jellyfin",
         json{
             {"enabled", c.jellyfin.enabled},
             {"server_url", c.jellyfin.server_url},
             {"api_key", c.jellyfin.api_key},
             {"user_id", c.jellyfin.user_id},
             {"default_playlist", c.jellyfin.default_playlist},
             {"use_favorites", c.jellyfin.use_favorites},
             {"shuffle", c.jellyfin.shuffle},
         }},
        {"external_audio",
         json{
             {"enabled", c.external_audio.enabled},
             {"endpoint_id", c.external_audio.endpoint_id},
             {"media_session_id", c.external_audio.media_session_id},
         }},
        {"spotify",
         json{
             {"enabled", c.spotify.enabled},
             {"librespot_path", path_s(c.spotify.librespot_path)},
             {"cache_dir", path_s(c.spotify.cache_dir)},
         }},
         {"online_radio",
         json{
             {"enabled", c.online_radio.enabled},
             {"default_station_index", c.online_radio.default_station_index},
             {"stations", [&c]() {
                  json arr = json::array();
                  for (const auto& st : c.online_radio.stations) {
                      arr.push_back({{"name", st.name},
                                     {"url", st.url},
                                     {"favicon", st.favicon},
                                     {"tags", st.tags},
                                     {"country", st.country},
                                     {"codec", st.codec},
                                     {"bitrate", st.bitrate},
                                     {"uuid", st.uuid},
                                     {"favorite", st.favorite}});
                  }
                  return arr;
              }()}
         }},
        {"vanilla_radio",
         json{
             {"enabled", c.vanilla_radio.enabled},
         }},
        {"audio",
         json{
             {"output_gain", c.audio.output_gain},
         }},
        {"playback",
         json{
             {"race_start_playback", c.playback.race_start_playback},
             {"quick_station_skip", c.playback.quick_station_skip},
             {"volume_normalization", c.playback.volume_normalization},
             {"equalizer_enabled", c.playback.equalizer_enabled},
             {"equalizer_bands", c.playback.equalizer_bands},
             {"force_stereo_audio", c.playback.force_stereo_audio},
             {"prebuffer_next_track", c.playback.prebuffer_next_track},
         }},
    };
}

template <class T> T pull(const json& tbl, const char* k, T fallback) {
    if (auto it = tbl.find(k); it != tbl.end() && !it->is_null()) {
        try {
            return it->get<T>();
        } catch (...) {}
    }
    return fallback;
}
std::filesystem::path pull_path(const json& tbl, const char* k,
                                const std::filesystem::path& fallback) {
    auto s = pull<std::string>(tbl, k, path_s(fallback));
    return s.empty() ? std::filesystem::path{} : std::filesystem::path{s};
}

std::vector<std::filesystem::path> paths_from_json(const json& a) {
    std::vector<std::filesystem::path> out;
    if (a.is_array()) {
        for (const auto& e : a) {
            if (e.is_string()) {
                auto s = e.get<std::string>();
                if (!s.empty()) out.emplace_back(s);
            }
        }
    }
    return out;
}

LocalStation station_from_json(const json& j) {
    LocalStation s;
    s.name      = pull<std::string>(j, "name", "");
    s.recursive = pull<bool>(j, "recursive", true);
    s.order     = pull<std::string>(j, "order", s.order);
    s.grouping  = pull<std::string>(j, "grouping", s.grouping);
    s.repeat    = pull<std::string>(j, "repeat", s.repeat);
    if (auto it = j.find("roots"); it != j.end()) s.roots = paths_from_json(*it);
    if (auto it = j.find("excluded"); it != j.end()) s.excluded = paths_from_json(*it);
    return s;
}

// Deep-merge a partial JSON patch into Config. Absent keys keep their value.
void apply_patch(Config& c, const json& j) {
    if (auto it = j.find("general"); it != j.end()) {
        c.general.port            = pull(*it, "port", c.general.port);
        c.general.ring_buffer_mb  = pull(*it, "ring_buffer_mb", c.general.ring_buffer_mb);
        c.general.default_source  = pull(*it, "default_source", c.general.default_source);
        c.general.fallback_source = pull(*it, "fallback_source", c.general.fallback_source);
        c.general.ffmpeg_path     = pull_path(*it, "ffmpeg_path", c.general.ffmpeg_path);
    }
    if (auto it = j.find("local_files"); it != j.end()) {
        c.local_files.enabled        = pull(*it, "enabled", c.local_files.enabled);
        c.local_files.active_station = pull(*it, "active_station", c.local_files.active_station);
        if (auto sts = it->find("stations"); sts != it->end() && sts->is_array()) {
            std::vector<LocalStation> parsed;
            for (const auto& st : *sts) parsed.push_back(station_from_json(st));
            if (!parsed.empty()) c.local_files.stations = std::move(parsed);
        }
        if (auto fmts = it->find("supported_formats"); fmts != it->end() && fmts->is_array())
            c.local_files.supported_formats = fmts->get<std::vector<std::string>>();
    }
    if (auto it = j.find("youtube_music"); it != j.end()) {
        c.youtube_music.enabled      = pull(*it, "enabled", c.youtube_music.enabled);
        c.youtube_music.cookies_path = pull_path(*it, "cookies_path", c.youtube_music.cookies_path);
        c.youtube_music.yt_dlp_path  = pull_path(*it, "yt_dlp_path", c.youtube_music.yt_dlp_path);
        c.youtube_music.active_station = pull(*it, "active_station", c.youtube_music.active_station);
        if (auto sts = it->find("stations"); sts != it->end() && sts->is_array()) {
            std::vector<YouTubeStation> parsed;
            for (const auto& st : *sts) parsed.push_back(yt_station_from_json(st));
            c.youtube_music.stations = std::move(parsed);
        }
        c.youtube_music.shuffle = pull(*it, "shuffle", c.youtube_music.shuffle);
    }
    if (auto it = j.find("jellyfin"); it != j.end()) {
        c.jellyfin.enabled          = pull(*it, "enabled", c.jellyfin.enabled);
        c.jellyfin.server_url       = pull(*it, "server_url", c.jellyfin.server_url);
        c.jellyfin.api_key          = pull(*it, "api_key", c.jellyfin.api_key);
        c.jellyfin.user_id          = pull(*it, "user_id", c.jellyfin.user_id);
        c.jellyfin.default_playlist = pull(*it, "default_playlist", c.jellyfin.default_playlist);
        c.jellyfin.use_favorites    = pull(*it, "use_favorites", c.jellyfin.use_favorites);
        c.jellyfin.shuffle          = pull(*it, "shuffle", c.jellyfin.shuffle);
    }
    if (auto it = j.find("external_audio"); it != j.end()) {
        c.external_audio.enabled     = pull(*it, "enabled", c.external_audio.enabled);
        c.external_audio.endpoint_id = pull(*it, "endpoint_id", c.external_audio.endpoint_id);
        c.external_audio.media_session_id =
            pull(*it, "media_session_id", c.external_audio.media_session_id);
    }
    if (auto it = j.find("spotify"); it != j.end()) {
        c.spotify.enabled        = pull(*it, "enabled", c.spotify.enabled);
        c.spotify.librespot_path = pull_path(*it, "librespot_path", c.spotify.librespot_path);
        c.spotify.cache_dir      = pull_path(*it, "cache_dir", c.spotify.cache_dir);
    }
    if (auto it = j.find("online_radio"); it != j.end()) {
        c.online_radio.enabled = pull(*it, "enabled", c.online_radio.enabled);
        c.online_radio.default_station_index = pull(*it, "default_station_index", c.online_radio.default_station_index);
        if (auto st = it->find("stations"); st != it->end() && st->is_array()) {
            c.online_radio.stations.clear();
            for (const auto& item : *st) {
                RadioStation rs;
                rs.name     = item.value("name", "");
                rs.url      = item.value("url", "");
                rs.favicon  = item.value("favicon", "");
                rs.tags     = item.value("tags", "");
                rs.country  = item.value("country", "");
                rs.codec    = item.value("codec", "");
                rs.bitrate  = item.value("bitrate", 0);
                if (rs.bitrate < 0) rs.bitrate = 0;
                rs.uuid     = item.value("uuid", "");
                rs.favorite = item.value("favorite", false);
                c.online_radio.stations.push_back(std::move(rs));
            }
        }
        if (c.online_radio.stations.empty()) {
            c.online_radio.default_station_index = 0;
        } else if (c.online_radio.default_station_index >= c.online_radio.stations.size()) {
            c.online_radio.default_station_index = 0; // or last valid index
        }
    }
    if (auto it = j.find("vanilla_radio"); it != j.end()) {
        c.vanilla_radio.enabled = pull(*it, "enabled", c.vanilla_radio.enabled);
    }
    if (auto it = j.find("audio"); it != j.end()) {
        c.audio.output_gain = pull(*it, "output_gain", c.audio.output_gain);
    }
    if (auto it = j.find("playback"); it != j.end()) {
        auto rs = pull<std::string>(*it, "race_start_playback", c.playback.race_start_playback);
        if (rs == "next" || rs == "restart" || rs == "ignore")
            c.playback.race_start_playback = std::move(rs);
        c.playback.quick_station_skip =
            pull(*it, "quick_station_skip", c.playback.quick_station_skip);
        c.playback.volume_normalization =
            pull(*it, "volume_normalization", c.playback.volume_normalization);
        c.playback.force_stereo_audio =
            pull(*it, "force_stereo_audio", c.playback.force_stereo_audio);
        c.playback.prebuffer_next_track =
            pull(*it, "prebuffer_next_track", c.playback.prebuffer_next_track);
        c.playback.equalizer_enabled = pull(*it, "equalizer_enabled", c.playback.equalizer_enabled);
        if (auto bands = it->find("equalizer_bands"); bands != it->end() && bands->is_array()) {
            for (std::size_t i = 0; i < c.playback.equalizer_bands.size() && i < bands->size();
                 ++i) {
                float b = (*bands)[i].get<float>();
                if (b < -6.f) b = -6.f;
                if (b > 6.f) b = 6.f;
                c.playback.equalizer_bands[i] = b;
            }
        }
    }
}

struct Request {
    std::string method;
    std::string path;
    std::string body;
};

constexpr std::string_view status_text(int code) noexcept {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 502: return "Bad Gateway";
        default: return "Internal Server Error";
    }
}

constexpr std::string_view mime_for(std::string_view path) noexcept {
    auto ends = [&](std::string_view ext) {
        return path.size() >= ext.size() && path.ends_with(ext);
    };
    if (ends(".html")) return "text/html";
    if (ends(".css")) return "text/css";
    if (ends(".js")) return "application/javascript";
    if (ends(".svg")) return "image/svg+xml";
    if (ends(".png")) return "image/png";
    if (ends(".json")) return "application/json";
    if (ends(".woff2")) return "font/woff2";
    if (ends(".woff")) return "font/woff";
    return "text/plain";
}

size_t header_size_t(std::string_view headers, std::string_view name_lower) {
    for (size_t i = 0; i + name_lower.size() < headers.size(); ++i) {
        bool match = true;
        for (size_t k = 0; k < name_lower.size(); ++k) {
            if (static_cast<char>(std::tolower(static_cast<unsigned char>(headers[i + k]))) !=
                name_lower[k]) {
                match = false;
                break;
            }
        }
        if (!match) continue;
        size_t p = i + name_lower.size();
        while (p < headers.size() && (headers[p] == ':' || headers[p] == ' ' || headers[p] == '\t'))
            ++p;
        size_t v = 0;
        while (p < headers.size() && std::isdigit(static_cast<unsigned char>(headers[p])))
            v = v * 10 + static_cast<size_t>(headers[p++] - '0');
        return v;
    }
    return 0;
}

bool read_request(SOCKET client, Request& req) {
    std::string raw;
    raw.reserve(1024);
    std::array<char, 4096> buf{};

    size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        int r = recv(client, buf.data(), static_cast<int>(buf.size()), 0);
        if (r <= 0) return false;
        raw.append(buf.data(), static_cast<size_t>(r));
        header_end = raw.find("\r\n\r\n");
        if (raw.size() > 64 * 1024) return false; // header bomb guard
    }

    std::istringstream first{raw.substr(0, raw.find("\r\n"))};
    if (!(first >> req.method >> req.path)) return false;

    const std::string_view headers{raw.data(), header_end};
    const size_t content_length = header_size_t(headers, "content-length");

    req.body.assign(raw, header_end + 4, std::string::npos);
    while (req.body.size() < content_length) {
        const size_t need = std::min<size_t>(buf.size(), content_length - req.body.size());
        int r             = recv(client, buf.data(), static_cast<int>(need), 0);
        if (r <= 0) break;
        req.body.append(buf.data(), static_cast<size_t>(r));
    }
    return true;
}

void send_all(SOCKET client, std::string_view data) {
    while (!data.empty()) {
        int n = send(client, data.data(), static_cast<int>(data.size()), 0);
        if (n <= 0) return;
        data.remove_prefix(static_cast<size_t>(n));
    }
}

void send_response(SOCKET client, int code, std::string_view body,
                   std::string_view content_type = "application/json") {
    std::ostringstream resp;
    resp << "HTTP/1.1 " << code << ' ' << status_text(code) << "\r\n"
         << "Content-Type: " << content_type << "\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Access-Control-Allow-Origin: *\r\n"
         << "Access-Control-Allow-Methods: GET, POST, PUT, OPTIONS\r\n"
         << "Access-Control-Allow-Headers: Content-Type\r\n"
         << "Connection: close\r\n\r\n";
    auto headers = resp.str();
    send_all(client, headers);
    send_all(client, body);
}

bool serve_file(SOCKET client, const std::filesystem::path& file) {
    std::ifstream f{file, std::ios::binary};
    if (!f) return false;
    std::ostringstream buf;
    buf << f.rdbuf();
    send_response(client, 200, buf.str(), mime_for(file.string()));
    return true;
}

} // namespace

struct HttpServer::Impl {
    AudioSourceManager& mgr;
    fmod_bridge::DSPBridge& bridge;
    ConfigStore& store;
    DependencyManager& deps;
    std::filesystem::path ui_dist;
    std::atomic<bool> stopping{false};
    SOCKET srv_sock = INVALID_SOCKET;
    std::thread thr;

    Impl(AudioSourceManager& m, fmod_bridge::DSPBridge& b, ConfigStore& s, DependencyManager& d,
         uint16_t port, std::filesystem::path dist)
        : mgr{m}, bridge{b}, store{s}, deps{d}, ui_dist{std::move(dist)} {
        thr = std::thread{[this, port] { run(port); }};
    }
    ~Impl() {
        stopping.store(true, std::memory_order_release);
        if (srv_sock != INVALID_SOCKET) closesocket(srv_sock);
        if (thr.joinable()) thr.join();
    }

    json build_sources() const {
        auto* a        = mgr.active();
        json available = json::array();
        for (auto* s : mgr.sources_snapshot()) available.push_back(source_to_json(s));
        return json{
            {"active", a ? std::string{a->name()} : ""},
            {"available", std::move(available)},
        };
    }

    json build_state() const {
        auto* a = mgr.active();
        return json{
            {"game", json{{"attached", true}, {"injector_ready", true}}},
            {"audio",
             json{
                 {"active", bridge.mode() == fmod_bridge::DSPMode::pcm},
                 {"native_dsp_mode", mode_string(bridge.mode())},
                 {"output_gain", bridge.gain()},
                 {"underruns", bridge.underruns()},
                 {"calls", bridge.call_count()},
                 {"buffer_len", bridge.last_buffer_len()},
                 {"out_channels", bridge.last_out_channels()},
                 {"ring_avail", mgr.ring().readable()},
                 {"ring_capacity", mgr.ring().capacity()},
             }},
            {"sources", build_sources()},
            {"track", a ? track_to_json(a->current_track()) : json::object()},
            {"errors", json::array()},
        };
    }

    IAudioSource* find(std::string_view name) const { return mgr.find(name); }
    template <class T> T* find_typed(std::string_view name) const {
        return dynamic_cast<T*>(find(name));
    }

    void run(uint16_t port) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            log::error("[http] WSAStartup failed");
            return;
        }

        srv_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (srv_sock == INVALID_SOCKET) {
            log::error("[http] socket() failed");
            WSACleanup();
            return;
        }
        BOOL yes = TRUE;
        setsockopt(srv_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes),
                   sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);

        uint16_t bound = port;
        addr.sin_port  = htons(bound);
        if (bind(srv_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            bound         = static_cast<uint16_t>(port + 1);
            addr.sin_port = htons(bound);
            if (bind(srv_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                log::error("[http] could not bind port {} or {}", port, bound);
                closesocket(srv_sock);
                srv_sock = INVALID_SOCKET;
                WSACleanup();
                return;
            }
        }

        if (listen(srv_sock, 16) != 0) {
            log::error("[http] listen() failed");
            closesocket(srv_sock);
            srv_sock = INVALID_SOCKET;
            WSACleanup();
            return;
        }
        log::info("[http] listening on http://0.0.0.0:{} (LAN-reachable)", bound);

        while (!stopping.load(std::memory_order_acquire)) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(srv_sock, &rfds);
            timeval tv{0, 200'000};
            if (select(0, &rfds, nullptr, nullptr, &tv) <= 0) continue;

            SOCKET client = accept(srv_sock, nullptr, nullptr);
            if (client == INVALID_SOCKET) continue;
            handle(client);
            closesocket(client);
        }

        WSACleanup();
    }

    void handle(SOCKET client) {
        Request req;
        if (!read_request(client, req)) return;

        auto ok = [&](const json& j = {}) {
            std::string body = j.empty() ? std::string{R"({"ok":true})"}
                                         : j.dump(-1, ' ', false, json::error_handler_t::replace);
            send_response(client, 200, body);
        };
        auto fail = [&](int code, std::string_view msg) {
            send_response(client, code, json{{"error", std::string{msg}}}.dump());
        };

        try {
            dispatch(client, req, ok, fail);
        } catch (...) {
            fail(400, "bad request body");
        }
    }

    template <class Ok, class Fail>
    void dispatch(SOCKET client, const Request& req, Ok&& ok, Fail&& fail) {
        const auto& m = req.method;
        const auto& p = req.path;

        if (m == "OPTIONS") return send_response(client, 200, "", "text/plain");

        if (m == "GET" && p == "/api/state") return ok(build_state());
        if (m == "GET" && p == "/api/events") return send_event_snapshot(client);
        if (m == "GET" && p == "/api/sources") return ok(build_sources());
        if (m == "GET" && p.starts_with("/api/artwork")) {
            // ?v=<token> only busts the browser cache; the active source
            // always serves its current track's art.
            if (auto* a = mgr.active()) {
                if (auto img = a->artwork())
                    return send_response(client, 200, img->bytes, img->mime);
            }
            return fail(404, "no artwork");
        }
        if (m == "GET" && p == "/api/config") return ok(config_to_json(store.snapshot()));
        if (m == "GET" && p == "/api/deps") {
            json tools = json::array();
            for (const auto& d : deps.snapshot()) {
                tools.push_back(json{
                    {"name", d.name},
                    {"present", d.managed_present},
                    {"downloading", d.downloading},
                    {"downloaded_bytes", d.downloaded_bytes},
                    {"total_bytes", d.total_bytes},
                    {"error", d.error},
                });
            }
            return ok(json{{"tools", std::move(tools)}});
        }
        if (m == "POST" && p == "/api/deps/refresh") {
            deps.retry();
            return ok();
        }
        if (m == "GET" && p == "/api/source/local_files/queue") {
            auto* lf = find_typed<sources::LocalFileSource>("local_files");
            if (!lf) return fail(404, "local_files not registered");
            auto snap   = lf->queue_snapshot();
            json tracks = json::array();
            for (const auto& e : snap.entries) {
                tracks.push_back(
                    json{{"index", e.index}, {"title", e.title}, {"folder", e.folder}});
            }
            return ok(json{{"cursor", snap.cursor}, {"tracks", tracks}});
        }
        if (m == "POST" && p == "/api/fs/browse") {
            auto j                    = req.body.empty() ? json::object() : json::parse(req.body);
            std::filesystem::path dir = j.value("path", std::string{});
            json entries              = json::array();
            for (const auto& e : sources::enumerate_dir(dir)) {
                entries.push_back(
                    json{{"name", e.name}, {"path", e.path}, {"has_children", e.has_children}});
            }
            std::string parent;
            if (!dir.empty()) {
                auto pp = dir.parent_path();
                if (pp != dir) parent = path_s(pp); // at a drive root, fall back to the drive list
            }
            return ok(json{{"parent", parent}, {"path", path_s(dir)}, {"entries", entries}});
        }
        if (m == "GET" && p == "/api/source/local_files/stations") {
            auto* lf  = find_typed<sources::LocalFileSource>("local_files");
            auto snap = store.snapshot();
            return ok(json{
                {"stations", stations_to_json(snap.local_files.stations)},
                {"active_station", snap.local_files.active_station},
                {"track_count", lf ? lf->track_count() : 0},
            });
        }
        if (m == "PUT" && p == "/api/source/local_files/stations") {
            auto j = json::parse(req.body);
            store.patch([&](Config& c) {
                if (auto sts = j.find("stations"); sts != j.end() && sts->is_array()) {
                    std::vector<LocalStation> parsed;
                    for (const auto& st : *sts) parsed.push_back(station_from_json(st));
                    if (!parsed.empty()) c.local_files.stations = std::move(parsed);
                }
                if (auto a = j.find("active_station"); a != j.end() && a->is_string())
                    c.local_files.active_station = a->get<std::string>();
            });
            auto* lf = find_typed<sources::LocalFileSource>("local_files");
            return ok(json{{"track_count", lf ? lf->track_count() : 0}});
        }
        if (m == "POST" && p == "/api/source/local_files/activate") {
            auto* lf = find_typed<sources::LocalFileSource>("local_files");
            if (!lf) return fail(404, "local_files not registered");
            auto name             = json::parse(req.body).value("name", std::string{});
            const bool was_active = (mgr.active() == lf);
            // patch -> apply_config -> set_config rebuilds for the new station.
            store.patch([&](Config& c) { c.local_files.active_station = name; });
            if (was_active) mgr.ring().drain();
            lf->play();
            mgr.switch_to("local_files");
            return ok(json{{"track_count", lf->track_count()}});
        }
        if (m == "POST" && p == "/api/source/local_files/play" && !req.body.empty()) {
            auto* lf = find_typed<sources::LocalFileSource>("local_files");
            if (!lf) return fail(404, "local_files not registered");
            auto idx              = json::parse(req.body).at("index").get<std::size_t>();
            const bool was_active = (mgr.active() == lf);
            if (!lf->jump_to(idx)) return fail(400, "index out of range");
            if (was_active) mgr.ring().drain();
            mgr.switch_to("local_files");
            return ok();
        }
        if (m == "POST" && p == "/api/source/local_files/reshuffle") {
            auto* lf = find_typed<sources::LocalFileSource>("local_files");
            if (!lf) return fail(404, "local_files not registered");
            lf->reshuffle();
            return ok();
        }

        if (m == "PUT" && p == "/api/config") {
            auto patch = json::parse(req.body);
            store.patch([&](Config& c) { apply_patch(c, patch); });
            return ok(config_to_json(store.snapshot()));
        }

        if (m == "POST" && p == "/api/config/reload") {
            store.reload();
            return ok(config_to_json(store.snapshot()));
        }
        if (m == "POST" && p == "/api/source/switch") {
            auto src = json::parse(req.body).at("source").get<std::string>();
            return mgr.switch_to(src) ? ok() : fail(404, "unknown source");
        }
        if (m == "GET" && p == "/api/external_audio/devices") {
            json devices = json::array();
            for (const auto& d : sources::enumerate_external_audio_devices()) {
                devices.push_back(json{
                    {"id", d.id},
                    {"name", d.name},
                    {"is_default", d.is_default},
                });
            }
            auto snap     = store.snapshot();
            json sessions = json::array();
            for (const auto& session : sources::enumerate_external_audio_media_sessions(
                     snap.external_audio.media_session_id)) {
                sessions.push_back(json{
                    {"id", session.id},
                    {"name", session.name},
                    {"is_current", session.is_current},
                    {"is_selected", session.is_selected},
                });
            }
            return ok(json{
                {"enabled", snap.external_audio.enabled},
                {"endpoint_id", snap.external_audio.endpoint_id},
                {"media_session_id", snap.external_audio.media_session_id},
                {"media_sessions_available", sources::external_audio_media_sessions_available()},
                {"media_sessions", sessions},
                {"devices", devices},
            });
        }
        if (m == "PUT" && p == "/api/external_audio/config") {
            auto body           = req.body.empty() ? json::object() : json::parse(req.body);
            auto snap_before    = store.snapshot();
            const auto endpoint = body.value("endpoint_id", snap_before.external_audio.endpoint_id);
            const auto media_session_id =
                body.value("media_session_id", snap_before.external_audio.media_session_id);
            const auto enabled = body.value("enabled", snap_before.external_audio.enabled);
            // store.patch() notifies the bridge observer synchronously, which
            // (un)registers the source and pushes the new config via set_config.
            store.patch([&](Config& c) {
                c.external_audio.enabled          = enabled;
                c.external_audio.endpoint_id      = endpoint;
                c.external_audio.media_session_id = media_session_id;
            });
            auto snap = store.snapshot();
            return ok(json{{"enabled", snap.external_audio.enabled},
                           {"endpoint_id", snap.external_audio.endpoint_id},
                           {"media_session_id", snap.external_audio.media_session_id}});
        }
        if (m == "POST" && p == "/api/source/youtube_music/cast") {
            auto* yt = find_typed<sources::YouTubeMusicSource>("youtube_music");
            if (!yt) return fail(404, "youtube_music not registered");
            auto url              = json::parse(req.body).at("url").get<std::string>();
            if (url.empty()) return fail(400, "url required");
            const bool was_active = (mgr.active() == yt);
            yt->stop();
            yt->set_target(std::move(url));
            if (was_active) mgr.ring().drain();
            yt->play();
            mgr.switch_to("youtube_music");
            return ok();
        }
        if (m == "POST" && p == "/api/source/youtube_music/shuffle") {
            auto* yt = find_typed<sources::YouTubeMusicSource>("youtube_music");
            if (!yt) return fail(404, "youtube_music not registered");
            auto shuffle = json::parse(req.body).at("shuffle").get<bool>();
            yt->set_shuffle(shuffle);
            store.patch([shuffle](Config& c) { c.youtube_music.shuffle = shuffle; });
            return ok();
        }
        if (m == "GET" && p == "/api/source/youtube_music/stations") {
            auto snap = store.snapshot();
            return ok(json{
                {"stations", yt_stations_to_json(snap.youtube_music.stations)},
                {"active_station", snap.youtube_music.active_station}
            });
        }
        if (m == "PUT" && p == "/api/source/youtube_music/stations") {
            auto j = json::parse(req.body);
            store.patch([&](Config& c) {
                if (auto sts = j.find("stations"); sts != j.end() && sts->is_array()) {
                    std::vector<YouTubeStation> parsed;
                    for (const auto& st : *sts) parsed.push_back(yt_station_from_json(st));
                    c.youtube_music.stations = std::move(parsed);
                }
                if (auto a = j.find("active_station"); a != j.end() && a->is_string())
                    c.youtube_music.active_station = a->get<std::string>();
            });
            auto* yt = find_typed<sources::YouTubeMusicSource>("youtube_music");
            if (yt) yt->set_config(store.snapshot().youtube_music);
            return ok();
        }
        if (m == "POST" && p == "/api/source/youtube_music/activate") {
            auto* yt = find_typed<sources::YouTubeMusicSource>("youtube_music");
            if (!yt) return fail(404, "youtube_music not registered");
            auto name             = json::parse(req.body).value("name", std::string{});
            const bool was_active = (mgr.active() == yt);
            store.patch([&](Config& c) { c.youtube_music.active_station = name; });
            yt->set_active_station(name);
            if (was_active) mgr.ring().drain();
            yt->play();
            mgr.switch_to("youtube_music");
            return ok();
        }
        if (m == "GET" && p == "/api/source/youtube_music/queue") {
            auto* yt = find_typed<sources::YouTubeMusicSource>("youtube_music");
            if (!yt) return fail(404, "youtube_music not registered");
            auto snap   = yt->queue_snapshot();
            json tracks = json::array();
            for (const auto& e : snap.entries) {
                tracks.push_back(
                    json{{"index", e.index}, {"title", e.title}, {"artist", e.artist}, {"url", e.url}});
            }
            return ok(json{{"cursor", snap.cursor}, {"tracks", tracks}});
        }

        if (m == "POST" && p == "/api/source/youtube_music/play" && !req.body.empty()) {
            auto* yt = find_typed<sources::YouTubeMusicSource>("youtube_music");
            if (!yt) return fail(404, "youtube_music not registered");
            auto idx              = json::parse(req.body).at("index").get<std::size_t>();
            const bool was_active = (mgr.active() == yt);
            if (!yt->jump_to(idx)) return fail(400, "index out of range");
            if (was_active) mgr.ring().drain();
            mgr.switch_to("youtube_music");
            return ok();
        }
        if (m == "POST" && p == "/api/source/jellyfin/cast") {
            auto* jf = find_typed<sources::JellyfinSource>("jellyfin");
            if (!jf) return fail(404, "jellyfin not registered");
            auto playlist_id = json::parse(req.body).value("playlist_id", std::string{});
            if (playlist_id.empty()) return fail(400, "playlist_id required");
            const bool was_active = (mgr.active() == jf);
            if (!jf->cast(std::move(playlist_id))) return fail(502, "jellyfin fetch failed");
            if (was_active) mgr.ring().drain();
            mgr.switch_to("jellyfin");
            return ok();
        }
        if (m == "POST" && p == "/api/source/online_radio/cast") {
            auto* rd = find_typed<sources::OnlineRadioSource>("online_radio");
            if (!rd) return fail(404, "online_radio not registered");
            auto body = json::parse(req.body);
            auto url  = body.value("url", std::string{});
            if (url.empty()) return fail(400, "url required");
            if (!sources::OnlineRadioSource::is_streamable_url(url)) {
                return fail(400, "only http/https urls are allowed");
            }

            const bool was_active = (mgr.active() == rd);
            rd->stop();
            rd->set_target(std::move(url), body.value("name", std::string{}),
                           body.value("logo", std::string{}));
            if (was_active) mgr.ring().drain();
            rd->play();
            mgr.switch_to("online_radio");
            return ok();
        }
        if (m == "POST" && p == "/api/source/local_files/rescan") {
            auto* lf = find_typed<sources::LocalFileSource>("local_files");
            if (!lf) return fail(404, "local_files not registered");
            // Re-apply the stored config; set_config rescans the active station.
            lf->set_config(store.snapshot().local_files);
            return ok(json{{"track_count", lf->track_count()}});
        }
        if (m == "POST" && p == "/api/options") {
            auto j = json::parse(req.body);
            if (auto it = j.find("output_gain"); it != j.end()) {
                float g = std::clamp(it->get<float>(), 0.0f, 1.0f);
                bridge.set_gain(g);
                store.patch([&](Config& c) { c.audio.output_gain = g; });
            }
            return ok();
        }

        // Generic transport: POST /api/source/<name>/{play|pause|stop|next|previous}
        constexpr std::string_view prefix = "/api/source/";
        if (m == "POST" && p.starts_with(prefix)) {
            const std::string_view rest = std::string_view{p}.substr(prefix.size());
            const auto slash            = rest.find('/');
            if (slash == std::string_view::npos) return fail(400, "invalid route");
            const std::string_view name = rest.substr(0, slash);
            const std::string_view act  = rest.substr(slash + 1);
            auto* s                     = find(name);
            if (!s) return fail(404, "unknown source");
            const bool is_active = (s == mgr.active());
            if (act == "play") {
                s->play();
            } else if (act == "pause") {
                s->pause();
            } else if (act == "stop") {
                s->stop();
                if (is_active) mgr.ring().drain();
            } else if (act == "next") {
                s->next();
                if (is_active) mgr.ring().drain();
            } else if (act == "previous") {
                s->previous();
                if (is_active) mgr.ring().drain();
            } else {
                return fail(404, "unknown action");
            }
            return ok();
        }

        // SPA static-file fallback.
        if (m == "GET" && !ui_dist.empty()) {
            const std::string rel = (p == "/") ? "index.html" : p.substr(1);
            if (serve_file(client, ui_dist / rel)) return;
            if (p.find('.') == std::string::npos && serve_file(client, ui_dist / "index.html"))
                return;
        }
        fail(404, "not found");
    }

    void send_event_snapshot(SOCKET client) const {
        auto body = build_state().dump(-1, ' ', false, json::error_handler_t::replace);
        std::string evt;
        evt.reserve(body.size() + 256);
        evt.append("HTTP/1.1 200 OK\r\n"
                   "Content-Type: text/event-stream\r\n"
                   "Cache-Control: no-store\r\n"
                   "Access-Control-Allow-Origin: *\r\n"
                   "Connection: close\r\n\r\n"
                   "retry: 1000\ndata: ");
        evt.append(body);
        evt.append("\n\n");
        send_all(client, evt);
    }
};

HttpServer::HttpServer(AudioSourceManager& mgr, fmod_bridge::DSPBridge& bridge, ConfigStore& cfg,
                       uint16_t port, std::filesystem::path ui_dist, DependencyManager& deps)
    : impl_{std::make_unique<Impl>(mgr, bridge, cfg, deps, port, std::move(ui_dist))} {}

HttpServer::~HttpServer() = default;

} // namespace fh6::http
