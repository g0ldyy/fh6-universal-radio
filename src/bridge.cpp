// Bridge entry point. Wires up config, sources, the FMOD DSP, and the HTTP
// dashboard, then parks the thread the DLL spawned us on.

#include "fh6/log.hpp"
#include "fh6/config.hpp"
#include "fh6/config_store.hpp"
#include "fh6/deps.hpp"
#include "fh6/audio_source_manager.hpp"
#include "fh6/fmod/dsp_bridge.hpp"
#include "fh6/fmod/dsp_control_loop.hpp"
#include "fh6/fmod/pe_image.hpp"
#include "fh6/fmod/texture_injector.hpp"
#include "fh6/http/http_server.hpp"
#include "fh6/sources/local_file_source.hpp"
#include "fh6/sources/external_audio_source.hpp"
#include "fh6/sources/youtube_music_source.hpp"
#include "fh6/sources/jellyfin_source.hpp"
#include "fh6/sources/spotify_source.hpp"
#include "fh6/worker/worker_client.hpp"
#include "fh6/sources/online_radio_source.hpp"
#include "fh6/sources/vanilla_radio_source.hpp"

#include <windows.h>
#include <array>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

namespace fh6 {

namespace {

std::filesystem::path module_directory(HMODULE self) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path{buf}.parent_path();
}

bool host_is_game(const std::filesystem::path& dll_dir) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    const std::filesystem::path exe{std::wstring_view{buf, n}};

    std::error_code ec;
    if (!std::filesystem::equivalent(exe.parent_path(), dll_dir, ec)) return false;

    std::wstring name = exe.filename().wstring();
    for (wchar_t& c : name) c = static_cast<wchar_t>(std::towlower(c));
    return name != L"gamelaunchhelper.exe";
}

SpotifyConfig anchor_spotify(SpotifyConfig sp, const std::filesystem::path& data_dir) {
    if (!sp.cache_dir.empty() && sp.cache_dir.is_relative()) sp.cache_dir = data_dir / sp.cache_dir;
    return sp;
}

// Swap blank binary paths for the auto-downloaded copies. Returned to sources
// only -- the stored config keeps the user's blanks so the dashboard still
// shows "auto".
Config with_resolved_bins(Config c, const DependencyManager& deps) {
    c.general.ffmpeg_path       = deps.resolve(Tool::ffmpeg, c.general.ffmpeg_path);
    c.youtube_music.yt_dlp_path = deps.resolve(Tool::yt_dlp, c.youtube_music.yt_dlp_path);
    c.spotify.librespot_path    = deps.resolve(Tool::librespot, c.spotify.librespot_path);
    std::string texconv_path    = deps.resolve(Tool::texconv, "").string();

    log::info("[bridge] Resolved ffmpeg path to: {}", c.general.ffmpeg_path.string());
    log::info("[bridge] Resolved yt-dlp path to: {}", c.youtube_music.yt_dlp_path.string());
    log::info("[bridge] Resolved librespot path to: {}", c.spotify.librespot_path.string());
    log::info("[bridge] Resolved texconv path to: {}", texconv_path);
    return c;
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in{p, std::ios::binary};
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return std::move(ss).str();
}

// Refuse to start if the bundled webui has been stripped of credits or
// donation links. Keeps forks honest about the GPLv3 attribution requirement
// and stops the project from being re-skinned with the funding links removed.
bool verify_ui_credits(const std::filesystem::path& ui_dir) {
    const auto index = ui_dir / "index.html";
    const auto html  = slurp(index);
    if (html.empty()) {
        log::error("[bridge] webui index.html missing or unreadable at {}", index.string());
        return false;
    }
    constexpr std::array<std::string_view, 5> required = {
        "g0ldyy",                                // author attribution
        "GPLv3",                                 // license credit
        "github.com/sponsors/g0ldyy",            // GitHub Sponsors link
        "ko-fi.com/g0ldyy",                      // Ko-fi link
        "github.com/g0ldyy/fh6-universal-radio", // upstream repo link
    };
    for (auto needle : required) {
        if (html.find(needle) == std::string::npos) {
            log::error("[bridge] webui is missing required credit/donation marker '{}' -- "
                       "refusing to start. See LICENSE (GPLv3) for attribution requirements.",
                       std::string{needle});
            return false;
        }
    }
    return true;
}

} // namespace

void run_bridge(HMODULE self) noexcept {
    const auto dir = module_directory(self);
    if (!host_is_game(dir)) return; // never spawn a bridge outside the game itself

    if (HANDLE guard = CreateMutexW(nullptr, TRUE, L"Local\\fh6-universal-radio-bridge");
        guard && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(guard);
        return;
    }

    const auto data_dir = dir / "fh6-radio";
    std::error_code ec;
    std::filesystem::create_directories(data_dir, ec);

    log::init(data_dir / "bridge.log");
    log::info("[bridge] FH6 Universal Radio starting; data_dir={}", data_dir.string());

    const auto ui_dir = data_dir / "ui";
    if (!verify_ui_credits(ui_dir)) {
        log::error("[bridge] aborting startup: webui credits/donation links check failed");
        return;
    }

    ConfigStore store{data_dir / "config.toml", load_config(data_dir / "config.toml")};
    auto cfg = store.snapshot();

    auto img = fmod_bridge::parse(reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr)));
    if (!img.valid()) {
        log::error("[bridge] failed to parse host PE image; aborting");
        return;
    }
    fmod_bridge::FMODFns fns;
    if (!fmod_bridge::resolve_fmod_signatures(img, fns)) {
        log::warn("[bridge] some FMOD signatures unresolved -- DSP injection disabled");
    }

    const std::size_t ring_bytes = static_cast<std::size_t>(cfg.general.ring_buffer_mb) << 20;
    AudioSourceManager mgr{ring_bytes};

    DependencyManager deps{data_dir / "bin"};

    TextureInjector::instance().set_deps(&deps);
    TextureInjector::instance().set_config_store(&store);

    // Worker process: delegates CreateProcess calls to a small external exe
    // so the fork() Wine performs is cheap (~5 MB) instead of copying the
    // game's multi-GB page table.  Falls back to direct spawn if absent.
    auto worker = std::make_shared<worker::WorkerClient>();
    {
        auto worker_exe = data_dir / "fh6-radio-worker.exe";
        if (!std::filesystem::exists(worker_exe))
            worker_exe = dir / "fh6-radio" / "fh6-radio-worker.exe";
        
        if (worker->start(worker_exe, {{L"RUST_LOG", L"librespot_playback::player=debug,librespot_metadata=trace"}})) {
            log::info("[bridge] worker process started");

            TextureInjector::instance().set_worker_client(worker);
        } else {
            log::warn("[bridge] worker process unavailable -- falling back to direct spawn");
        }
    }

    // Register/unregister sources to match the enabled flags. Called at
    // startup and on every config change so toggling enabled adds/removes
    // the dashboard tile live, without a game restart.
    auto sync_sources = [&mgr, &data_dir, &worker](const Config& c) {
        if (c.local_files.enabled && !mgr.find("local_files")) {
            auto src = std::make_unique<sources::LocalFileSource>(
                c.local_files, c.general.ffmpeg_path, data_dir / "local_index.json", worker.get());
            if (src->initialize()) mgr.register_source(std::move(src));
        } else if (!c.local_files.enabled && mgr.find("local_files")) {
            mgr.unregister_source("local_files");
        }
        if (c.youtube_music.enabled && !mgr.find("youtube_music")) {
            auto src = std::make_unique<sources::YouTubeMusicSource>(c.youtube_music,
                                                                     c.general.ffmpeg_path,
                                                                     worker.get());
            if (src->initialize()) mgr.register_source(std::move(src));
        } else if (!c.youtube_music.enabled && mgr.find("youtube_music")) {
            mgr.unregister_source("youtube_music");
        }
        if (c.jellyfin.enabled && !mgr.find("jellyfin")) {
            auto src = std::make_unique<sources::JellyfinSource>(c.jellyfin, c.general.ffmpeg_path,
                                                                  worker.get());
            if (src->initialize()) mgr.register_source(std::move(src));
        } else if (!c.jellyfin.enabled && mgr.find("jellyfin")) {
            mgr.unregister_source("jellyfin");
        }
        if (c.online_radio.enabled && !mgr.find("online_radio")) {
            auto src = std::make_unique<sources::OnlineRadioSource>(c.online_radio,
                                                                    c.general.ffmpeg_path, worker.get());
            if (src->initialize()) mgr.register_source(std::move(src));
        } else if (!c.online_radio.enabled && mgr.find("online_radio")) {
            mgr.unregister_source("online_radio");
        }

        if (c.external_audio.enabled && !mgr.find("external_audio")) {
            auto src = std::make_unique<sources::ExternalAudioSource>(c.external_audio);
            if (src->initialize()) mgr.register_source(std::move(src));
        } else if (!c.external_audio.enabled && mgr.find("external_audio")) {
            mgr.unregister_source("external_audio");
        }
        if (c.spotify.enabled && !mgr.find("spotify")) {
            auto src = std::make_unique<sources::SpotifySource>(anchor_spotify(c.spotify, data_dir),
                                                                c.general.ffmpeg_path, worker.get());
            if (src->initialize()) mgr.register_source(std::move(src));
        } else if (!c.spotify.enabled && mgr.find("spotify")) {
            mgr.unregister_source("spotify");
        }
        if (c.vanilla_radio.enabled && !mgr.find("vanilla_radio")) {
            auto src = std::make_unique<sources::VanillaRadioSource>();
            if (src->initialize()) mgr.register_source(std::move(src));
        } else if (!c.vanilla_radio.enabled && mgr.find("vanilla_radio")) {
            mgr.unregister_source("vanilla_radio");
        }
    };

    sync_sources(with_resolved_bins(cfg, deps));

    if (!mgr.switch_to(cfg.general.default_source) && !mgr.switch_to(cfg.general.fallback_source)) {
        auto snap = mgr.sources_snapshot();
        if (snap.empty()) {
            log::warn("[bridge] no sources registered");
        } else if (snap.size() == 1) {
            if (!mgr.switch_to(snap[0]->name())) {
                log::error("[bridge] failed to switch to sole registered source '{}'",
                           snap[0]->name());
            }
        } else {
            log::warn("[bridge] configured default/fallback sources not found among {} registered "
                      "sources",
                      snap.size());
        }
    }

    fmod_bridge::DSPBridge bridge{mgr, fns};
    bridge.set_gain(cfg.audio.output_gain);
    bridge.set_force_stereo_audio(cfg.playback.force_stereo_audio);

    // cycle the playlist for whichever source is currently active
    auto cycle_station = [&store, &mgr]() -> bool {
        bool changed = false;
        store.patch([&mgr, &changed](Config& c) {
            auto* active = mgr.active();
            if (!active) return;
            auto name = active->name();
            
            if (name == "local_files" && c.local_files.stations.size() > 1) {
                size_t idx = 0;
                for (size_t i = 0; i < c.local_files.stations.size(); ++i) {
                    if (c.local_files.stations[i].name == c.local_files.active_station) { idx = i; break; }
                }
                c.local_files.active_station = c.local_files.stations[(idx + 1) % c.local_files.stations.size()].name;
                changed = true;
            }
            else if (name == "youtube_music" && c.youtube_music.stations.size() > 1) {
                size_t idx = 0;
                for (size_t i = 0; i < c.youtube_music.stations.size(); ++i) {
                    if (c.youtube_music.stations[i].name == c.youtube_music.active_station) { idx = i; break; }
                }
                c.youtube_music.active_station = c.youtube_music.stations[(idx + 1) % c.youtube_music.stations.size()].name;
                changed = true;
            }
            else if (name == "jellyfin" && c.jellyfin.stations.size() > 1) {
                size_t idx = 0;
                for (size_t i = 0; i < c.jellyfin.stations.size(); ++i) {
                    if (c.jellyfin.stations[i].name == c.jellyfin.active_station) { idx = i; break; }
                }
                c.jellyfin.active_station = c.jellyfin.stations[(idx + 1) % c.jellyfin.stations.size()].name;
                changed = true;
            }
            else if (name == "online_radio" && c.online_radio.stations.size() > 1) {
                c.online_radio.default_station_index = (c.online_radio.default_station_index + 1) % c.online_radio.stations.size();
                changed = true;
            }
        });
        return changed;
    };

    std::unique_ptr<fmod_bridge::ControlLoop> ctrl;
    if (fns.ready()) {
        ctrl = std::make_unique<fmod_bridge::ControlLoop>(bridge, img, cfg.playback,
                                                          cfg.audio.output_gain, cycle_station);
    }

    for (auto* s : mgr.sources_snapshot()) s->set_playback_options(cfg.playback);

    auto apply_config = [&bridge, &mgr, &data_dir, &deps, sync_sources,
                         ctrl_ptr = ctrl.get()](const Config& raw) {
        const Config c = with_resolved_bins(raw, deps);
        sync_sources(c);
        if (!mgr.active()) {
            if (!mgr.switch_to(c.general.default_source) &&
                !mgr.switch_to(c.general.fallback_source)) {
                auto snap = mgr.sources_snapshot();
                if (snap.size() == 1) {
                    if (!mgr.switch_to(snap[0]->name())) {
                        log::error("[bridge] failed to switch to sole registered source '{}'",
                                   snap[0]->name());
                    }
                }
            }
        }

        // Push the gain to both: the control loop's ramper otherwise snaps
        // the bridge value back to its own cached target on the next tick.
        bridge.set_gain(c.audio.output_gain);
        bridge.set_force_stereo_audio(c.playback.force_stereo_audio);
        if (ctrl_ptr) ctrl_ptr->set_configured_gain(c.audio.output_gain);
        if (auto* local = dynamic_cast<sources::LocalFileSource*>(mgr.find("local_files"))) {
            local->set_ffmpeg_path(c.general.ffmpeg_path);
            local->set_config(c.local_files);
            if (mgr.active() == local && local->track_count() > 0 &&
                local->playback_state() != PlaybackState::playing) {
                local->play();
            }
        }
        if (auto* yt = dynamic_cast<sources::YouTubeMusicSource*>(mgr.find("youtube_music"))) {
            yt->set_ffmpeg_path(c.general.ffmpeg_path);
            yt->set_config(c.youtube_music); 
            yt->set_yt_dlp_path(c.youtube_music.yt_dlp_path);
            yt->set_shuffle(c.youtube_music.shuffle);
            if (mgr.active() == yt && yt->playback_state() != PlaybackState::playing) {
                yt->play();
            }
        }
        if (auto* jf = dynamic_cast<sources::JellyfinSource*>(mgr.find("jellyfin"))) {
            jf->set_ffmpeg_path(c.general.ffmpeg_path);
            jf->set_config(c.jellyfin);
            if (mgr.active() == jf && jf->playback_state() != PlaybackState::playing) {
                jf->play();
            }
        }
        if (auto* ext = dynamic_cast<sources::ExternalAudioSource*>(mgr.find("external_audio"))) {
            ext->set_config(c.external_audio);
        }
        if (auto* sp = dynamic_cast<sources::SpotifySource*>(mgr.find("spotify"))) {
            sp->set_config(anchor_spotify(c.spotify, data_dir), c.general.ffmpeg_path);
        }
        if (auto* rd = dynamic_cast<sources::OnlineRadioSource*>(mgr.find("online_radio"))) {
            rd->set_ffmpeg_path(c.general.ffmpeg_path);
            rd->set_config(c.online_radio);
            if (mgr.active() == rd && rd->playback_state() != PlaybackState::playing) {
                rd->play();
            }
        }

        for (auto* s : mgr.sources_snapshot()) s->set_playback_options(c.playback);
        if (ctrl_ptr) ctrl_ptr->push_playback_options(c.playback);
    };
    store.on_change(apply_config);

    // Re-resolve binary paths into the live sources once a download lands.
    deps.start([&store, &apply_config] { apply_config(store.snapshot()); });

    http::HttpServer http{mgr, bridge, store, cfg.general.port, ui_dir, deps};
    log::info("[bridge] running on port {}", cfg.general.port);

    for (;;) Sleep(60'000);
}

} // namespace fh6