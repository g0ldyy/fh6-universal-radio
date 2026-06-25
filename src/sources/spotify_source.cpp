#include "fh6/sources/spotify_source.hpp"
#include "fh6/sources/external_media_session.hpp"
#include "fh6/log.hpp"
#include "fh6/subprocess.hpp"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fh6::sources {

namespace {

using subprocess::create_kill_on_close_job;
using subprocess::describe_launch_failure;
using subprocess::open_nul;
using subprocess::open_stderr_log;
using subprocess::quote;
using subprocess::spawn_in_job;
using subprocess::stderr_log_path;

// 48 kHz * 2 ch * 2 bytes = 192 000 B/s, so PCM position maps to time at 192 B/ms.
constexpr uint64_t kBytesPerMs        = 192;
constexpr std::size_t kMaxBufferBytes = 28800; // 150 ms of in-flight PCM
constexpr std::size_t kPipeChunk      = 4096;  // OS-minimum pipe / read granularity
// A track-load event arriving while the previous track is still this far from
// its end means the user skipped inside the Spotify app -- adopt it at once.
constexpr uint64_t kExternalSkipGuardMs = 32000;

// Press-and-release one extended media key (next/prev fallback).
void send_media_key(WORD vk) {
    INPUT ip[2] = {};
    ip[0].type = ip[1].type = INPUT_KEYBOARD;
    ip[0].ki.wVk = ip[1].ki.wVk = vk;
    ip[0].ki.dwFlags            = KEYEVENTF_EXTENDEDKEY;
    ip[1].ki.dwFlags            = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
    SendInput(2, ip, sizeof(INPUT));
}

// Undo Rust's `{:?}` string escaping for the common cases that appear in
// track/album/artist names (\" and \\); other escapes keep their literal char.
std::string unescape_debug(const std::string& s) {
    if (s.find('\\') == std::string::npos) return s;
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) ++i;
        out.push_back(s[i]);
    }
    return out;
}

} // namespace

struct SpotifySource::Pipe {
    worker::WorkerClient* worker = nullptr;
    uint32_t pipeline_id = 0;
    
    HANDLE job       = nullptr;
    HANDLE proc_spot = nullptr;
    HANDLE proc_ff   = nullptr;
    HANDLE read_pipe = nullptr;
    // log parsing
    HANDLE err_pipe = nullptr;
    HANDLE log_file = nullptr;
    std::string err_buf;
    bool ended = false;

    // tracks the total PCM bytes pumped to calculate UI time syncing
    uint64_t bytes_consumed = 0;

    // gapless & prefetch tracking
    std::string pending_title;
    std::string pending_artist;
    std::string pending_album;
    std::string pending_cover_url;
    uint64_t pending_duration_ms = 0;
    uint64_t track_duration_ms   = 0;
    bool has_pending             = false;
    int stall_ticks              = 0;
    bool force_next_metadata     = false;
    bool awaiting_first_track    = true; // no real track adopted yet

    enum class MetaContext : std::uint8_t { None, Track, Album, Artist };
    MetaContext meta_context = MetaContext::None;
    bool expecting_name      = false;

    // cover parsing states
    bool in_cover_group      = false;
    bool expecting_cover_id  = false;
    std::vector<uint8_t> next_meta_cover_bytes;

    std::string next_meta_title;
    std::string next_meta_artist;
    std::string next_meta_album;
    std::string next_meta_cover_url;

    ~Pipe() {
        if (read_pipe) CloseHandle(read_pipe);
        if (err_pipe) CloseHandle(err_pipe);
        if (log_file && log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file);
        
        if (worker && pipeline_id) worker->kill_pipeline(pipeline_id);
        subprocess::reap(proc_spot);
        subprocess::reap(proc_ff);
        if (job) CloseHandle(job);
    }
};

SpotifySource::SpotifySource(SpotifyConfig cfg, std::filesystem::path ffmpeg_path, worker::WorkerClient* worker)
    : cfg_{std::move(cfg)}, ffmpeg_path_{std::move(ffmpeg_path)}, worker_{worker} {
    info_.title  = "Ready to Cast";
    info_.artist = "Spotify Connect";
}

SpotifySource::~SpotifySource() {
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
}

bool SpotifySource::initialize() {
    if (!cfg_.enabled) return false;

    // librespot stores credentials here; a cache dir we can't create means
    // auth can never persist, so fail fast rather than run half-configured.
    std::error_code ec;
    std::filesystem::create_directories(cfg_.cache_dir, ec);
    if (ec) {
        log::warn("[spotify] cannot create cache dir {} ({})", cfg_.cache_dir.string(),
                ec.message());
        return false;
    }
    return true;
}

void SpotifySource::set_config(SpotifyConfig cfg, std::filesystem::path ffmpeg_path) {
    // Stored only; a live pipe keeps its current paths and picks the new ones up
    // on the next start. Restarting here would cut playback on unrelated config
    // saves (the dashboard fires on_change for any field change).
    std::scoped_lock lk{mu_};
    cfg_         = std::move(cfg);
    ffmpeg_path_ = std::move(ffmpeg_path);
}

void SpotifySource::set_playback_options(const PlaybackConfig& opts) {
    std::scoped_lock lk{mu_};
    volume_normalization_ = opts.volume_normalization;
}

void SpotifySource::shutdown() noexcept {
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
}

bool SpotifySource::cache_exists() const {
    auto creds_path = cfg_.cache_dir / "credentials.json";
    std::error_code ec;
    return std::filesystem::exists(creds_path, ec); // noexcept overload: auth_state() is noexcept
}

AuthState SpotifySource::auth_state() const noexcept {
    return cache_exists() ? AuthState::authenticated : AuthState::needs_auth;
}

std::string SpotifySource::auth_instructions() const {
    return "1. Ensure your PC and phone are on the same Wi-Fi network.\n"
        "2. Open the Spotify app on your phone.\n"
        "3. Tap the 'Devices' icon and select 'FH6 Universal Radio'.\n"
        "Once connected, credentials will automatically save to the cache folder.";
}

void SpotifySource::start_pipe_locked() {
    stop_pipe_locked();

    auto pipe = std::make_unique<Pipe>();
    
    const auto spot = cfg_.librespot_path.empty() ? L"librespot" : cfg_.librespot_path.wstring();
    const std::wstring ff = ffmpeg_path_.empty() ? std::wstring{L"ffmpeg"} : ffmpeg_path_.wstring();
    const auto cache      = cfg_.cache_dir.wstring();

    std::wstring spot_cmd = quote(spot) + L" --name \"FH6 Universal Radio\"" + L" --bitrate 320" +
                            L" --backend pipe" + L" --initial-volume 100" + L" --cache " +
                            quote(cache) + L" --disable-audio-cache";

    if (volume_normalization_) {
        spot_cmd += L" --enable-volume-normalisation";
    }

    // librespot defaults to 44100Hz s16le. We must resample to 48000Hz for FH6.
    // added flags to disable FFmpeg internal buffering for perfect UI sync
    std::wstring ff_cmd = quote(ff) + L" -loglevel error" + L" -fflags nobuffer -flags low_delay" +
                        L" -blocksize 4096" + // force micro-block processing
                        L" -f s16le -ar 44100 -ac 2 -i pipe:0" + L" -flush_packets 1" +
                        L" -f s16le -acodec pcm_s16le -ar 48000 -ac 2 pipe:1";

    // request debug logs for the player module to intercept Seek events & metadata
    SetEnvironmentVariableW(L"RUST_LOG",
                            L"librespot_playback::player=debug,librespot_metadata=trace");

    if (worker_ && worker_->alive()) {
        // spawn the pipeline via worker_client
        // use meta_stderr_idx = 0 to capture librespot stderr directly
        // skip capture_stderr_meta, which would otherwise capture ffmpeg (the last process) stderr
        // set all pipeline buffers to 4096 bytes to minimize buffering and reduce residual backlog
        if (auto result = worker_->spawn_pipeline({spot_cmd, ff_cmd}, L"", false, 0, 4096); result.ok) {
            SetEnvironmentVariableW(L"RUST_LOG", nullptr);

            pipe->worker      = worker_;
            pipe->pipeline_id = result.pipeline_id;
            pipe->read_pipe   = result.pcm_pipe;
            pipe->err_pipe    = result.meta_pipe;
            pipe->log_file    = open_stderr_log(); // keep extraneous trace logging functional 
            pipe_             = std::move(pipe);

            info_.title  = "Streaming via Spotify Connect";
            info_.artist = "Spotify";
            info_.album.clear();
            info_.artwork_url.clear();
            state_.store(PlaybackState::playing, std::memory_order_release);

            log::info("[spotify] librespot pipe started via worker (listening on network)");
            return;
        }
        log::warn("[spotify] worker spawn failed -- falling back to direct spawn");
    }

    pipe->job = create_kill_on_close_job();
    if (!pipe->job) {
        log::warn("[spotify] CreateJobObject failed ({})", GetLastError());
        return;
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE spot_out_r = nullptr, spot_out_w = nullptr;
    HANDLE spot_err_r = nullptr, spot_err_w = nullptr;
    HANDLE ff_out_r = nullptr, ff_out_w = nullptr;

    auto bail = [&] {
        if (spot_out_r) CloseHandle(spot_out_r);
        if (spot_out_w) CloseHandle(spot_out_w);
        if (spot_err_r) CloseHandle(spot_err_r);
        if (spot_err_w) CloseHandle(spot_err_w);
        if (ff_out_r) CloseHandle(ff_out_r);
        if (ff_out_w) CloseHandle(ff_out_w);
    };

    // Pipes are created inheritable (sa) but every end is cleared below except
    // the three librespot inherits (its stdin/stdout/stderr). librespot is
    // spawned first; if ffmpeg's stdout write end (ff_out_w) leaked into it, the
    // read end would never see EOF when ffmpeg exits. The ffmpeg-side ends are
    // re-enabled just before ffmpeg is spawned.
    // reduced pipe size to 4KB (OS minimum) to eliminate residual data backlog
    if (!CreatePipe(&spot_out_r, &spot_out_w, &sa, 4096)) {
        bail();
        return;
    }
    SetHandleInformation(spot_out_r, HANDLE_FLAG_INHERIT, 0);
    // create error pipe and ensure read end isn't passed to children
    if (!CreatePipe(&spot_err_r, &spot_err_w, &sa, 4096)) {
        bail();
        return;
    }
    SetHandleInformation(spot_err_r, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&ff_out_r, &ff_out_w, &sa, 4096)) {
        bail();
        return;
    }
    SetHandleInformation(ff_out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(ff_out_w, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul_in  = open_nul(GENERIC_READ);
    pipe->log_file = open_stderr_log(); // keep the log file open in our Pipe struct
    if (pipe->log_file && pipe->log_file != INVALID_HANDLE_VALUE)
        SetHandleInformation(pipe->log_file, HANDLE_FLAG_INHERIT, 0); // ffmpeg only, not librespot

    // pass spot_err_w to librespot instead of the raw file
    pipe->proc_spot = spawn_in_job(pipe->job, spot_cmd, nul_in, spot_out_w, spot_err_w);
    const DWORD ec_spot = pipe->proc_spot ? 0u : GetLastError();
    SetEnvironmentVariableW(L"RUST_LOG", nullptr);
    // librespot owns its inherited stdin/stdout/stderr now; drop the parent copies.
    CloseHandle(spot_out_w);
    spot_out_w = nullptr;
    CloseHandle(spot_err_w);
    spot_err_w = nullptr;
    if (nul_in) {
        CloseHandle(nul_in);
        nul_in = nullptr;
    }

    if (!pipe->proc_spot) {
        log::warn("[spotify] failed to launch librespot -- {} (check {})",
                describe_launch_failure(std::wstring{spot}, ec_spot, !cfg_.librespot_path.empty()),
                stderr_log_path().string());
        bail();
        return;
    }

    // librespot has inherited only its own std handles; now expose the
    // ffmpeg-side ends so ffmpeg (and only ffmpeg) inherits them.
    SetHandleInformation(spot_out_r, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(ff_out_w, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    if (pipe->log_file && pipe->log_file != INVALID_HANDLE_VALUE)
        SetHandleInformation(pipe->log_file, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    // FFmpeg can keep logging to the file directly
    pipe->proc_ff = spawn_in_job(pipe->job, ff_cmd, spot_out_r, ff_out_w, pipe->log_file);
    const DWORD ec_ff = pipe->proc_ff ? 0u : GetLastError();
    // log_file is long-lived; stop later CreateProcess calls from inheriting it.
    if (pipe->log_file && pipe->log_file != INVALID_HANDLE_VALUE)
        SetHandleInformation(pipe->log_file, HANDLE_FLAG_INHERIT, 0);
    CloseHandle(spot_out_r);
    spot_out_r = nullptr;
    CloseHandle(ff_out_w);
    ff_out_w = nullptr;

    if (!pipe->proc_ff) {
        log::warn("[spotify] failed to launch ffmpeg -- {} (check {})",
                describe_launch_failure(std::wstring{ff}, ec_ff, !ffmpeg_path_.empty()),
                stderr_log_path().string());
        bail(); // clean up unassigned pipe handles
        return;
    }

    pipe->err_pipe  = spot_err_r; // save our read handle
    pipe->read_pipe = ff_out_r;
    pipe_           = std::move(pipe);

    info_.title  = "Streaming via Spotify Connect";
    info_.artist = "Spotify";
    info_.album.clear();
    info_.artwork_url.clear();
    state_.store(PlaybackState::playing, std::memory_order_release);

    log::info("[spotify] librespot pipe started (listening on network)");
}

void SpotifySource::stop_pipe_locked() {
    pipe_.reset();
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

void SpotifySource::play() {
    std::scoped_lock lk{mu_};
    // A dead stream (ffmpeg/librespot exited) leaves pipe_ set with ended=true;
    // respawn so the user can recover without an explicit stop() first.
    if (!pipe_ || pipe_->ended) start_pipe_locked();

    state_.store(PlaybackState::playing, std::memory_order_release);
}

void SpotifySource::pause() { state_.store(PlaybackState::paused, std::memory_order_release); }

void SpotifySource::stop() {
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
}

void SpotifySource::transport_skip(bool forward) {
    // Prefer background SMTC control; fall back to synthesizing the media key.
    const bool sent =
        forward ? external_audio_media_session_next("") : external_audio_media_session_previous("");
    if (!sent) send_media_key(forward ? VK_MEDIA_NEXT_TRACK : VK_MEDIA_PREV_TRACK);

    std::scoped_lock lk{mu_};
    if (pipe_) {
        pipe_->bytes_consumed      = 0;
        pipe_->has_pending         = false; // clear any queued prefetch
        pipe_->track_duration_ms   = 0;     // don't wait on the old duration
        pipe_->force_next_metadata = true;  // adopt the next track immediately
    }
    info_.position_ms = 0;
}

void SpotifySource::next() { transport_skip(true); }
void SpotifySource::previous() { transport_skip(false); }

bool SpotifySource::restart_current() {
    previous();
    return true;
}

TrackInfo SpotifySource::current_track() const {
    std::scoped_lock lk{mu_};
    return info_;
}

void SpotifySource::pump(RingBuffer& ring) {
    // do not return early if paused - keep draining the pipe
    std::scoped_lock lk{mu_};
    Pipe* p = pipe_.get();
    if (!p || !p->read_pipe || p->ended) return;

    // check if currently playing or paused
    bool is_playing = (state_.load(std::memory_order_acquire) == PlaybackState::playing);

    // Adopt a track's metadata into the now-playing info
    auto apply_info = [&](const std::string& title, const std::string& artist,
                        const std::string& album, uint64_t dur, const std::string& cover_url) {
        info_.title          = title;
        info_.artist         = artist;
        info_.album          = album;
        info_.duration_ms    = dur;
        info_.artwork_url    = cover_url;
        p->track_duration_ms = dur;
        p->has_pending       = false;
    };

    // non-blocking parse of track metadata
    if (p->err_pipe) {
        DWORD err_avail = 0;
        int safety = 0; // limit log reads to prevent audio thread starvation

        // process a maximum of 16KB of logs per pump cycle
        while (safety++ < 16 &&
            PeekNamedPipe(p->err_pipe, nullptr, 0, nullptr, &err_avail, nullptr) &&
            err_avail > 0) {
            char buf[1024];
            DWORD to_read = std::min<DWORD>(err_avail, (DWORD)sizeof(buf));
            DWORD got     = 0;
            if (!ReadFile(p->err_pipe, buf, to_read, &got, nullptr) || got == 0) break;

            p->err_buf.append(buf, got);

            // process all complete lines
            size_t pos;
            while ((pos = p->err_buf.find('\n')) != std::string::npos) {
                std::string line = p->err_buf.substr(0, pos);
                p->err_buf.erase(0, pos + 1);

                // strip Windows carriage return if it exists
                if (!line.empty() && line.back() == '\r') line.pop_back();

                // detect if the line breaks out of the multiline metadata trace
                if (!line.empty() && line[0] == '[') {
                    if (line.find("TRACE librespot_metadata") == std::string::npos) {
                        p->meta_context = Pipe::MetaContext::None;
                    }
                }

                bool is_trace_start = line.find("TRACE librespot_metadata") != std::string::npos;
                bool is_trace_block = is_trace_start || p->meta_context != Pipe::MetaContext::None;

                if (!is_trace_block && p->log_file && p->log_file != INVALID_HANDLE_VALUE) {
                    std::string out_line = line + "\r\n";
                    DWORD w              = 0;
                    WriteFile(p->log_file, out_line.data(), static_cast<DWORD>(out_line.size()), &w,
                            nullptr);
                }

                if (is_trace_start &&
                    line.find("Received metadata: Track {") != std::string::npos) {
                    p->meta_context = Pipe::MetaContext::Track;
                    p->next_meta_title.clear();
                    p->next_meta_artist.clear();
                    p->next_meta_album.clear();
                    p->next_meta_cover_url.clear();
                    p->next_meta_cover_bytes.clear();
                    p->in_cover_group     = false;
                    p->expecting_cover_id = false;
                    p->expecting_name     = false;
                } else if (p->meta_context != Pipe::MetaContext::None) {
                    if (line.find("Album {") != std::string::npos) {
                        p->meta_context = Pipe::MetaContext::Album;
                    } else if (line.find("Artist {") != std::string::npos) {
                        p->meta_context = Pipe::MetaContext::Artist;
                    }

                    if (line.find("cover_group:") != std::string::npos) {
                        p->in_cover_group = true;
                    } else if (line.find("AudioFile") != std::string::npos) {
                        // The audio file list comes after the album's cover_group;
                        // leaving the group resets state so a 20-byte audio file_id
                        // is not mis-read as a cover image. A later cover_group line
                        // re-arms the flag, so this is safe regardless of ordering.
                        p->in_cover_group     = false;
                        p->expecting_cover_id = false;
                        p->next_meta_cover_bytes.clear();
                    }

                    // librespot prints the cover as a pretty-debug byte array;
                    // intercept the first one in the cover group and hex-encode
                    // it into the Spotify CDN image URL.
                    if (line.find("file_id: Some(") != std::string::npos) {
                        if (p->in_cover_group && p->next_meta_cover_url.empty())
                            p->expecting_cover_id = true;
                    } else if (p->expecting_cover_id) {
                        if (line.find(']') != std::string::npos) {
                            p->expecting_cover_id = false;
                            // a Spotify Image ID is a 20-byte SHA1 -> 40 hex chars
                            if (p->next_meta_cover_bytes.size() == 20) {
                                static constexpr char kHex[] = "0123456789abcdef";
                                std::string hex_id;
                                hex_id.reserve(40);
                                for (uint8_t b : p->next_meta_cover_bytes) {
                                    hex_id.push_back(kHex[b >> 4]);
                                    hex_id.push_back(kHex[b & 0x0F]);
                                }
                                p->next_meta_cover_url = "https://i.scdn.co/image/" + hex_id;
                            }
                            p->next_meta_cover_bytes.clear(); // retry the next image on a bad parse
                        } else if (size_t d = line.find_first_of("0123456789");
                                d != std::string::npos) {
                            try {
                                int val = std::stoi(line.substr(d));
                                if (val >= 0 && val <= 255)
                                    p->next_meta_cover_bytes.push_back(static_cast<uint8_t>(val));
                            } catch (...) {}
                        }
                    } else if (line.find("name: Some(") != std::string::npos) {
                        p->expecting_name = true;
                    } else if (p->expecting_name) {
                        size_t start = line.find_first_of('"');
                        size_t end   = line.find_last_of('"');
                        if (start != std::string::npos && end != std::string::npos && start < end) {
                            std::string val =
                                unescape_debug(line.substr(start + 1, end - start - 1));

                            if (p->meta_context == Pipe::MetaContext::Track &&
                                p->next_meta_title.empty()) {
                                p->next_meta_title = val;
                            } else if (p->meta_context == Pipe::MetaContext::Album &&
                                    p->next_meta_album.empty()) {
                                p->next_meta_album = val;
                            } else if (p->meta_context == Pipe::MetaContext::Artist) {
                                // librespot repeats artists across track/album; de-dup whole names
                                const std::string token = ", " + val + ", ";
                                if ((", " + p->next_meta_artist + ", ").find(token) ==
                                    std::string::npos) {
                                    if (!p->next_meta_artist.empty()) p->next_meta_artist += ", ";
                                    p->next_meta_artist += val;
                                }
                            }
                        }
                        p->expecting_name = false;
                    }
                }

                // catch seek events from librespot to sync UI timer on scrub
                const std::string seek_marker = "command=Seek(";
                size_t seek_pos               = line.find(seek_marker);
                if (seek_pos != std::string::npos) {
                    size_t end = line.find(')', seek_pos);
                    if (end != std::string::npos) {
                        try {
                            uint64_t parsed_seek =
                                std::stoull(line.substr(seek_pos + seek_marker.length(),
                                                        end - (seek_pos + seek_marker.length())));
                            p->bytes_consumed = parsed_seek * kBytesPerMs;
                        } catch (...) {}
                        continue; // parsed successfully, move to next line
                    }
                }

                // look for the track load event signature
                const std::string marker = "librespot_playback::player] <";
                size_t m_pos             = line.find(marker);
                if (m_pos != std::string::npos) {
                    size_t start = m_pos + marker.length();
                    size_t end   = line.find("> (", start);
                    if (end != std::string::npos) {
                        std::string parsed_title = line.substr(start, end - start);

                        // extract the duration e.g. "> (___ ms) loaded"
                        uint64_t parsed_duration = 0;
                        size_t ms_start          = end + 3;
                        size_t ms_end            = line.find(" ms) loaded", ms_start);
                        if (ms_end != std::string::npos) {
                            try {
                                parsed_duration =
                                    std::stoull(line.substr(ms_start, ms_end - ms_start));
                            } catch (...) {}
                        }

                        std::string final_title =
                            p->next_meta_title.empty() ? parsed_title : p->next_meta_title;
                        std::string final_artist =
                            p->next_meta_artist.empty() ? "Spotify Connect" : p->next_meta_artist;
                        std::string final_album = p->next_meta_album; // can be empty
                        std::string final_cover = p->next_meta_cover_url;

                        // A load event while we're still >32 s from the current
                        // track's end means a manual in-app skip -- adopt it now.
                        const uint64_t track_bytes = p->track_duration_ms * kBytesPerMs;
                        const bool is_external_skip =
                            p->track_duration_ms > 0 &&
                            p->bytes_consumed + kExternalSkipGuardMs * kBytesPerMs < track_bytes;

                        // First track, an explicit skip, or an in-app skip: apply at once.
                        if (p->awaiting_first_track || p->force_next_metadata || is_external_skip) {
                            apply_info(final_title, final_artist, final_album, parsed_duration,
                                    final_cover);
                            p->bytes_consumed       = 0;
                            p->awaiting_first_track = false;
                            p->force_next_metadata  = false;
                            p->stall_ticks          = 0;
                        } else {
                            // queue it for the gapless transition
                            p->pending_title       = final_title;
                            p->pending_artist      = final_artist;
                            p->pending_album       = final_album;
                            p->pending_duration_ms = parsed_duration;
                            p->pending_cover_url   = final_cover;
                            p->has_pending         = true;
                        }
                    }
                }
            }
        }
    }

    // Paused: stop draining. The 4 KB pipe fills almost immediately, which
    // back-pressures ffmpeg and librespot -- so playback pauses without desync.
    if (!is_playing) return;

    DWORD avail = 0;
    if (!PeekNamedPipe(p->read_pipe, nullptr, 0, nullptr, &avail, nullptr)) {
        p->ended = true;
        return;
    }

    if (avail == 0) {
        p->stall_ticks++;
        // if pipe is dry for just 5 ticks (~80ms)
        if (p->stall_ticks > 5) {
            // the user manually skipped during the final 30 seconds of the song
            if (p->has_pending) {
                apply_info(p->pending_title, p->pending_artist, p->pending_album,
                        p->pending_duration_ms, p->pending_cover_url);
                p->bytes_consumed = 0;
            }
            // catch-all for extreme network lag / dead stream (stall for > 800ms)
            else if (p->stall_ticks > 50) {
                p->force_next_metadata = true;
            }
        }
    } else {
        p->stall_ticks = 0;
    }

    // natural gapless transition
    if (p->has_pending && p->track_duration_ms > 0) {
        uint64_t track_bytes = p->track_duration_ms * kBytesPerMs;
        if (p->bytes_consumed >= track_bytes) {
            apply_info(p->pending_title, p->pending_artist, p->pending_album,
                    p->pending_duration_ms, p->pending_cover_url);
            // carry the remainder so the timer stays exact
            p->bytes_consumed -= track_bytes;
            p->stall_ticks     = 0;
        }
    }

    while (avail > 0) {
        const std::size_t buffered = ring.readable();
        if (buffered >= kMaxBufferBytes) break;

        const std::size_t writable = ring.writable();
        if (writable < 4) break;

        auto want = std::min<std::size_t>(
            {static_cast<std::size_t>(avail), writable, kMaxBufferBytes - buffered, kPipeChunk});

        // request whole stereo S16 frames (4 bytes) so we never write half a
        // frame; the leftover unaligned bytes stay in the pipe for the next iteration.
        want &= ~std::size_t{3};
        if (!want) break;

        std::byte buf[kPipeChunk];
        DWORD got = 0;
        if (!ReadFile(p->read_pipe, buf, static_cast<DWORD>(want), &got, nullptr) || got == 0) {
            p->ended = true;
            return;
        }

        ring.write(buf, got);
        p->bytes_consumed += got; // track exact bytes pushed to the stream
        avail              = avail > got ? avail - got : 0;
    }

    // UI timer = bytes pushed minus what's still queued ahead in the ring.
    const uint64_t unplayed = ring.readable();
    info_.position_ms =
        p->bytes_consumed > unplayed ? (p->bytes_consumed - unplayed) / kBytesPerMs : 0;
}

} // namespace fh6::sources
