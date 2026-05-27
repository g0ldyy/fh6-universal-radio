#include "fh6/http/http_server.hpp"
#include "fh6/audio_source_manager.hpp"
#include "fh6/config_store.hpp"
#include "fh6/fmod/dsp_bridge.hpp"
#include "fh6/log.hpp"
#include "fh6/sources/local_file_source.hpp"
#include "fh6/sources/youtube_music_source.hpp"

#include <winsock2.h>

#define CPPHTTPLIB_NO_EXCEPTIONS
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
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
    if (auto* lf = dynamic_cast<sources::LocalFileSource*>(s))
        j["details"]["track_count"] = lf->track_count();
    return j;
}

std::string path_s(const std::filesystem::path& p) {
    return p.empty() ? std::string{} : p.string();
}

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
         }},
        {"audio",
         json{
             {"output_gain", c.audio.output_gain},
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

// Deep-merge a partial JSON patch into Config. Absent keys keep their value.
void apply_patch(Config& c, const json& j) {
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
    }
    if (auto it = j.find("audio"); it != j.end()) {
        c.audio.output_gain = pull(*it, "output_gain", c.audio.output_gain);
    }
}

void cors(httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
}
void ok(httplib::Response& res, const json& body = json::object()) {
    res.set_content(body.empty() ? std::string{R"({"ok":true})"} : body.dump(), "application/json");
}
void fail(httplib::Response& res, int status, std::string_view msg) {
    res.status = status;
    res.set_content(json{{"error", std::string{msg}}}.dump(), "application/json");
}

} // namespace

struct RawServer : httplib::Server {
    using httplib::Server::Server;
    void set_socket(socket_t sock) { svr_sock_ = sock; }
    bool start() { return listen_after_bind(); }
};

struct HttpServer::Impl {
    AudioSourceManager& mgr;
    fmod_bridge::DSPBridge& bridge;
    ConfigStore& store;
    std::filesystem::path ui_dist;
    RawServer svr;
    std::jthread thr;
    std::atomic<bool> stopping{false};

    Impl(AudioSourceManager& m, fmod_bridge::DSPBridge& b, ConfigStore& s, uint16_t port,
         std::filesystem::path dist)
        : mgr{m}, bridge{b}, store{s}, ui_dist{std::move(dist)} {
        wire_routes();
        thr = std::jthread([this, port](const std::stop_token&) {
            WSADATA wsa;
            WSAStartup(MAKEWORD(2, 2), &wsa);

            SOCKET sock                     = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            constexpr SOCKET kInvalidSocket = std::numeric_limits<SOCKET>::max();
            if (sock == kInvalidSocket) {
                log::error("[http] socket creation failed: {}", WSAGetLastError());
                return;
            }

            sockaddr_in addr{};
            addr.sin_family      = AF_INET;
            addr.sin_port        = htons(static_cast<u_short>(port));
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                log::error("[http] bind failed on 127.0.0.1:{} — {}", port, WSAGetLastError());
                closesocket(sock);
                return;
            }

            if (::listen(sock, SOMAXCONN) != 0) {
                log::error("[http] listen failed: {}", WSAGetLastError());
                closesocket(sock);
                return;
            }

            log::info("[http] listening on http://127.0.0.1:{}", port);
            svr.set_socket(sock);
            svr.start();
        });
    }
    ~Impl() {
        stopping.store(true);
        svr.stop();
    }

    json build_state() const {
        auto* a      = mgr.active();
        json sources = json::array();
        for (auto* s : mgr.sources_snapshot()) sources.push_back(source_to_json(s));
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
            {"sources",
             {
                 {"active", a ? std::string{a->name()} : ""},
                 {"available", std::move(sources)},
             }},
            {"track", a ? track_to_json(a->current_track()) : json::object()},
            {"errors", json::array()},
        };
    }

    IAudioSource* find(std::string_view name) const {
        for (auto* s : mgr.sources_snapshot())
            if (s->name() == name) return s;
        return nullptr;
    }
    template <class T> T* find_typed(std::string_view name) const {
        return dynamic_cast<T*>(find(name));
    }

    void wire_routes() {
        svr.Options(R"(/.*)", [](const httplib::Request&, httplib::Response& r) { cors(r); });

        // Live state
        svr.Get("/api/state", [this](const httplib::Request&, httplib::Response& r) {
            cors(r);
            ok(r, build_state());
        });
        svr.Get("/api/events", [this](const httplib::Request&, httplib::Response& r) {
            cors(r);
            r.set_header("Cache-Control", "no-store");
            r.set_chunked_content_provider(
                "text/event-stream", [this](std::size_t, httplib::DataSink& sink) {
                    std::string ev = "data: " + build_state().dump() + "\n\n";
                    if (!sink.write(ev.data(), ev.size())) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(750));
                    return !stopping.load(std::memory_order_acquire);
                });
        });

        // Source switching + transport
        svr.Get("/api/sources", [this](const httplib::Request&, httplib::Response& r) {
            cors(r);
            ok(r, build_state()["sources"]);
        });
        svr.Post("/api/source/switch", [this](const httplib::Request& q, httplib::Response& r) {
            cors(r);
            try {
                auto src = json::parse(q.body).at("source").get<std::string>();
                if (mgr.switch_to(src)) {
                    ok(r);
                } else {
                    fail(r, 404, "unknown source");
                }
            } catch (...) {
                fail(r, 400, "bad body");
            }
        });
        svr.Post(R"(/api/source/([^/]+)/(play|pause|stop|next|previous))",
                 [this](const httplib::Request& q, httplib::Response& r) {
                     cors(r);
                     auto* s = find(q.matches[1].str());
                     if (!s) {
                         fail(r, 404, "unknown source");
                         return;
                     }
                     const auto act       = q.matches[2].str();
                     const bool is_active = (s == mgr.active());
                     if (act == "play") {
                         s->play();
                     } else if (act == "pause") {
                         s->pause();
                         // Track-change actions drain the ring; otherwise FMOD keeps
                         // consuming up to ~20 s of stale PCM before the new track is heard.
                     } else if (act == "stop") {
                         s->stop();
                         if (is_active) mgr.ring().drain();
                     } else if (act == "next") {
                         s->next();
                         if (is_active) mgr.ring().drain();
                     } else if (act == "previous") {
                         s->previous();
                         if (is_active) mgr.ring().drain();
                     }
                     ok(r);
                 });

        // Source-specific runtime actions
        svr.Post("/api/source/youtube_music/cast",
                 [this](const httplib::Request& q, httplib::Response& r) {
                     cors(r);
                     auto* yt = find_typed<sources::YouTubeMusicSource>("youtube_music");
                     if (!yt) {
                         fail(r, 404, "youtube_music not registered");
                         return;
                     }
                     try {
                         auto url              = json::parse(q.body).at("url").get<std::string>();
                         const bool was_active = (mgr.active() == yt);
                         yt->set_target(std::move(url));
                         yt->stop();
                         if (was_active) mgr.ring().drain();
                         yt->play();
                         mgr.switch_to("youtube_music");
                         ok(r);
                     } catch (...) {
                         fail(r, 400, "bad body");
                     }
                 });
        svr.Post("/api/source/local_files/rescan", [this](const httplib::Request& q,
                                                          httplib::Response& r) {
            cors(r);
            auto* lf = find_typed<sources::LocalFileSource>("local_files");
            if (!lf) {
                fail(r, 404, "local_files not registered");
                return;
            }
            try {
                auto j = q.body.empty() ? json::object() : json::parse(q.body);
                std::filesystem::path dir;
                bool recursive = true;
                if (j.contains("music_dir")) dir = j.at("music_dir").get<std::string>();
                if (j.contains("recursive")) recursive = j.at("recursive").get<bool>();
                if (!dir.empty()) {
                    lf->set_directory(dir, recursive);
                    store.patch([&](Config& c) {
                        c.local_files.music_dir = dir;
                        c.local_files.recursive = recursive;
                    });
                } else {
                    auto snap = store.snapshot();
                    lf->set_directory(snap.local_files.music_dir, snap.local_files.recursive);
                }
                ok(r, json{{"track_count", lf->playlist_snapshot().size()}});
            } catch (...) {
                fail(r, 400, "bad body");
            }
        });
        svr.Get("/api/source/local_files/playlist",
                [this](const httplib::Request&, httplib::Response& r) {
                    cors(r);
                    auto* lf = find_typed<sources::LocalFileSource>("local_files");
                    if (!lf) {
                        fail(r, 404, "local_files not registered");
                        return;
                    }
                    ok(r, json{{"tracks", lf->playlist_snapshot()}});
                });

        // Config editing
        svr.Get("/api/config", [this](const httplib::Request&, httplib::Response& r) {
            cors(r);
            ok(r, config_to_json(store.snapshot()));
        });
        svr.Put("/api/config", [this](const httplib::Request& q, httplib::Response& r) {
            cors(r);
            try {
                auto patch = json::parse(q.body);
                store.patch([&](Config& c) { apply_patch(c, patch); });
                ok(r, config_to_json(store.snapshot()));
            } catch (const std::exception& e) {
                fail(r, 400, e.what());
            }
        });
        svr.Post("/api/config/reload", [this](const httplib::Request&, httplib::Response& r) {
            cors(r);
            store.reload();
            ok(r, config_to_json(store.snapshot()));
        });

        // Fast-path audio knobs (currently just output_gain, the slider).
        svr.Post("/api/options", [this](const httplib::Request& q, httplib::Response& r) {
            cors(r);
            try {
                auto j = json::parse(q.body);
                if (j.contains("output_gain")) {
                    float g = std::clamp(j.at("output_gain").get<float>(), 0.0f, 1.0f);
                    bridge.set_gain(g);
                    store.patch([&](Config& c) { c.audio.output_gain = g; });
                }
                ok(r);
            } catch (...) {
                fail(r, 400, "bad body");
            }
        });

        if (!ui_dist.empty() && std::filesystem::exists(ui_dist))
            svr.set_mount_point("/", ui_dist.string());
    }
};

HttpServer::HttpServer(AudioSourceManager& mgr, fmod_bridge::DSPBridge& bridge, ConfigStore& cfg,
                       uint16_t port, std::filesystem::path ui_dist)
    : impl_{std::make_unique<Impl>(mgr, bridge, cfg, port, std::move(ui_dist))} {}

HttpServer::~HttpServer() = default;

} // namespace fh6::http
