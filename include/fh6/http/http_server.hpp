#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

namespace fh6 {
class AudioSourceManager;
class ConfigStore;
namespace fmod_bridge {
class DSPBridge;
} // namespace fmod_bridge
} // namespace fh6

namespace fh6::http {

// Thin wrapper around cpp-httplib. All routes are CORS-enabled.
//
//   GET  /api/state                       unified live state
//   GET  /api/events                      SSE stream of /api/state snapshots
//
//   GET  /api/sources                     registered sources
//   POST /api/source/switch               body {"source":"name"}
//   POST /api/source/<name>/{play,pause,stop,next,previous}
//
//   POST /api/source/youtube_music/cast   body {"url":"..."}
//   POST /api/source/local_files/rescan   body {"music_dir":"...","recursive":bool}
//   GET  /api/source/local_files/playlist
//   GET  /api/source/roon/capture-devices
//
//   GET  /api/config                      full config.toml as JSON
//   PUT  /api/config                      deep-patch config; writes file + notifies
//   POST /api/config/reload               re-read config.toml from disk
//
//   POST /api/options                     fast-path knobs: output_gain, dsp_mode
//
//   GET  /                                bundled dashboard (vanilla SPA)
class HttpServer {
public:
    HttpServer(AudioSourceManager& mgr, fmod_bridge::DSPBridge& bridge, ConfigStore& cfg,
               uint16_t port, std::filesystem::path ui_dist);
    ~HttpServer();

    HttpServer(const HttpServer&)            = delete;
    HttpServer& operator=(const HttpServer&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fh6::http
