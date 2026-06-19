#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

namespace fh6 {
class AudioSourceManager;
class ConfigStore;
class DependencyManager;
namespace fmod_bridge {
class DSPBridge;
} // namespace fmod_bridge
} // namespace fh6

namespace fh6::http {

// Loopback-wildcard HTTP server (raw Winsock, single accept loop). All
// routes are CORS-enabled.
//
//   GET  /api/state                       unified live state
//   GET  /api/events                      SSE stream of /api/state snapshots
//
//   GET  /api/sources                     registered sources
//   POST /api/source/switch               body {"source":"name"}
//   POST /api/source/<name>/{play,pause,stop,next,previous}
//
//   POST /api/source/youtube_music/cast     body {"url":"..."}
//   POST /api/source/youtube_music/shuffle  body {"shuffle":true|false}
//   GET  /api/source/youtube_music/stations
//   PUT  /api/source/youtube_music/stations body {"stations":[...],"active_station":"..."}
//   POST /api/source/youtube_music/activate body {"name":"..."}
//   GET  /api/source/youtube_music/queue
//   POST /api/source/youtube_music/play     body {"index":N}
//
//   POST /api/source/online_radio/cast      body {"url":"...", ...}
//
//   POST /api/source/jellyfin/cast          body {"playlist_id":"..."}
//
//   GET  /api/external_audio/devices
//   PUT  /api/external_audio/config
//
//   POST /api/source/local_files/rescan
//
//   POST /api/fs/browse                   body {"path":"..."}; "" => drive list
//   GET  /api/source/local_files/stations stations + active_station + track_count
//   PUT  /api/source/local_files/stations body {"stations":[...],"active_station":"..."}
//   POST /api/source/local_files/activate body {"name":"..."} (switch source + play)
//   GET  /api/source/local_files/queue    {"cursor":N,"tracks":[{index,title,folder}]}
//   POST /api/source/local_files/play     body {"index":N}
//   POST /api/source/local_files/reshuffle
//   POST /api/source/local_files/rescan   re-scan the active station
//
//   GET  /api/config                      full config.toml as JSON
//   PUT  /api/config                      deep-patch config; writes file + notifies
//   POST /api/config/reload               re-read config.toml from disk
//
//   POST /api/options                     fast-path knobs: output_gain, dsp_mode
//
//   GET  /api/deps                        binary download status
//   POST /api/deps/refresh                retry any failed/missing downloads
//
//   GET  /                                bundled dashboard (vanilla SPA)
class HttpServer {
public:
    HttpServer(AudioSourceManager& mgr, fmod_bridge::DSPBridge& bridge, ConfigStore& cfg,
               uint16_t port, std::filesystem::path ui_dist, DependencyManager& deps);
    ~HttpServer();

    HttpServer(const HttpServer&)            = delete;
    HttpServer& operator=(const HttpServer&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fh6::http
