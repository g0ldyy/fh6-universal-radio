#include "fh6/sources/youtube_music_source.hpp"
#include "youtube_music_support.hpp"

#include "fh6/log.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace fh6::sources {

using namespace youtube_music_detail;

namespace {

bool tool_available(const std::filesystem::path& configured_path, const wchar_t* command_name) {
    if (!configured_path.empty()) {
        std::error_code ec;
        return std::filesystem::is_regular_file(configured_path, ec);
    }

    wchar_t resolved[MAX_PATH] = {};
    return SearchPathW(nullptr, command_name, L".exe", MAX_PATH, resolved, nullptr) != 0;
}

std::string youtube_tool_setup_message(const YouTubeMusicConfig& cfg) {
    const bool missing_yt = !tool_available(cfg.yt_dlp_path, L"yt-dlp");
    const bool missing_ff = !tool_available(cfg.ffmpeg_path, L"ffmpeg");
    if (!missing_yt && !missing_ff) return {};

    return "YouTube Music is enabled, but yt-dlp or ffmpeg could not be found. Run "
           "`winget install yt-dlp.yt-dlp`, `winget install Gyan.FFmpeg`, and "
           "`winget install DenoLand.Deno`, then keep yt-dlp/ffmpeg on PATH or set "
           "[youtube_music].yt_dlp_path and ffmpeg_path to the full .exe paths.";
}

} // namespace

YouTubeMusicSource::YouTubeMusicSource(YouTubeMusicConfig cfg) : cfg_{std::move(cfg)} {}

YouTubeMusicSource::~YouTubeMusicSource() { stop_pipe_locked(); }

bool YouTubeMusicSource::initialize() {
    if (!cfg_.enabled) return false;
    const auto setup_message = youtube_tool_setup_message(cfg_);
    if (!cfg_.default_playlist.empty()) {
        std::scoped_lock lk{mu_};
        target_url_ = cfg_.default_playlist;
    }
    {
        std::scoped_lock lk{mu_};
        auth_message_ = setup_message;
    }
    auth_.store(setup_message.empty() ? (cfg_.cookies_path.empty() ? AuthState::none_required
                                                                   : AuthState::authenticated)
                                      : AuthState::error,
                std::memory_order_release);
    return true;
}

void YouTubeMusicSource::shutdown() noexcept {
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
}

void YouTubeMusicSource::set_target(std::string url) {
    std::scoped_lock lk{mu_};
    target_url_ = std::move(url);
    // Invalidate queue so the next play() re-resolves against the new URL.
    queue_.clear();
    queue_idx_ = 0;
    queue_built_for_.clear();
}

void YouTubeMusicSource::set_shuffle(bool shuffle) {
    std::scoped_lock lk{mu_};
    cfg_.shuffle = shuffle;
    if (!queue_.empty() && queue_built_for_ == target_url_) {
        // Re-shuffle or re-sort the remaining queue (preserve current track)
        if (shuffle) {
            static thread_local std::mt19937 rng{std::random_device{}()};
            auto start = queue_.begin() + static_cast<std::ptrdiff_t>(queue_idx_ + 1);
            if (start < queue_.end()) {
                std::shuffle(start, queue_.end(), rng);
            }
        } else {
            // Re-sort the remaining queue (by URL, which is roughly chronological for playlists)
            auto start = queue_.begin() + static_cast<std::ptrdiff_t>(queue_idx_ + 1);
            if (start < queue_.end()) {
                std::sort(start, queue_.end());
            }
        }
    }
}

void YouTubeMusicSource::resolve_queue_locked() {
    if (target_url_.empty()) {
        queue_.clear();
        queue_idx_ = 0;
        queue_built_for_.clear();
        return;
    }
    if (queue_built_for_ == target_url_ && !queue_.empty()) return;

    queue_.clear();
    queue_idx_ = 0;

    if (!is_playlist_url(target_url_)) {
        queue_.push_back(target_url_);
        queue_built_for_ = target_url_;
        return;
    }

    // Playlist URL: enumerate IDs via --flat-playlist. Synchronous because the
    // HTTP cast handler can afford a few seconds; the alternative is a worker
    // thread + a longer-lived state machine for a marginal UX win.
    HANDLE job = create_kill_on_close_job();
    if (!job) {
        log::warn("[yt] resolve_queue: CreateJobObject failed ({})", GetLastError());
        return;
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) {
        CloseHandle(job);
        return;
    }
    SetHandleInformation(rd, 0, HANDLE_FLAG_INHERIT);

    HANDLE nul_in  = open_nul(GENERIC_READ);
    HANDLE err_log = open_stderr_log();
    const auto yt  = cfg_.yt_dlp_path.empty() ? L"yt-dlp" : cfg_.yt_dlp_path.wstring();

    std::wstring cmd = quote(yt) + L" --no-warnings --flat-playlist --skip-download "
                                   L"--print \"%(id)s\" ";
    if (!cfg_.cookies_path.empty())
        cmd += L"--cookies " + quote(cfg_.cookies_path.wstring()) + L" ";
    cmd += L"-- " + quote(widen(target_url_));

    HANDLE proc = spawn_in_job(job, cmd, nul_in, wr, err_log);
    // Capture the error before any other Win32 call clobbers it (CloseHandle resets it).
    const DWORD ec_yt = proc ? 0u : GetLastError();
    CloseHandle(wr);
    if (nul_in) CloseHandle(nul_in);
    if (err_log) CloseHandle(err_log);
    if (!proc) {
        CloseHandle(rd);
        CloseHandle(job);
        log::warn("[yt] resolve_queue: failed to launch yt-dlp -- {}",
                  describe_launch_failure(std::wstring{yt}, ec_yt, !cfg_.yt_dlp_path.empty()));
        return;
    }

    std::string raw = drain_to_eof(rd);
    CloseHandle(rd);
    CloseHandle(proc);
    CloseHandle(job); // KILL_ON_JOB_CLOSE -- ensures any straggling deno child dies

    for (std::size_t pos = 0; pos < raw.size();) {
        auto nl   = raw.find('\n', pos);
        auto end  = (nl == std::string::npos) ? raw.size() : nl;
        auto line = raw.substr(pos, end - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        if (!line.empty() && line != "NA") queue_.push_back(watch_url_for_id(line));
        pos = (nl == std::string::npos) ? raw.size() : nl + 1;
    }

    if (cfg_.shuffle && queue_.size() > 1) {
        static thread_local std::mt19937 rng{std::random_device{}()};
        std::shuffle(queue_.begin(), queue_.end(), rng);
    }

    queue_built_for_ = target_url_;
    if (queue_.empty() && is_playlist_url(target_url_)) {
        log::warn("[yt] resolved 0 tracks from {} -- check {} for yt-dlp errors "
                  "(private/deleted/geo-blocked playlist?)",
                  target_url_, stderr_log_path().string());
    } else {
        log::info("[yt] resolved {} track(s) from {}", queue_.size(), target_url_);
    }
}

void YouTubeMusicSource::start_pipe_locked() {
    stop_pipe_locked();
    resolve_queue_locked();
    if (queue_.empty()) return;
    if (queue_idx_ >= queue_.size()) queue_idx_ = 0;

    const std::string play_url = queue_[queue_idx_];

    auto pipe = std::make_unique<Pipe>();
    pipe->job = create_kill_on_close_job();
    if (!pipe->job) {
        log::warn("[yt] CreateJobObject failed ({})", GetLastError());
        return;
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE yt_out_r = nullptr, yt_out_w = nullptr;
    HANDLE ff_out_r = nullptr, ff_out_w = nullptr;
    HANDLE tl_out_r = nullptr, tl_out_w = nullptr;

    auto bail = [&] {
        if (yt_out_r) CloseHandle(yt_out_r);
        if (yt_out_w) CloseHandle(yt_out_w);
        if (ff_out_r) CloseHandle(ff_out_r);
        if (ff_out_w) CloseHandle(ff_out_w);
        if (tl_out_r) CloseHandle(tl_out_r);
        if (tl_out_w) CloseHandle(tl_out_w);
    };

    if (!CreatePipe(&yt_out_r, &yt_out_w, &sa, 1 << 20)) {
        bail();
        return;
    }
    SetHandleInformation(yt_out_r, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    if (!CreatePipe(&ff_out_r, &ff_out_w, &sa, 1 << 20)) {
        bail();
        return;
    }
    SetHandleInformation(ff_out_r, 0, HANDLE_FLAG_INHERIT);
    if (!CreatePipe(&tl_out_r, &tl_out_w, &sa, 1 << 16)) {
        bail();
        return;
    }
    SetHandleInformation(tl_out_r, 0, HANDLE_FLAG_INHERIT);

    HANDLE nul_in  = open_nul(GENERIC_READ);
    HANDLE err_log = open_stderr_log();

    const auto yt = cfg_.yt_dlp_path.empty() ? L"yt-dlp" : cfg_.yt_dlp_path.wstring();
    const auto ff = cfg_.ffmpeg_path.empty() ? L"ffmpeg" : cfg_.ffmpeg_path.wstring();

    // `--` terminates options so a URL starting with `-` isn't read as a flag.
    // `--no-playlist` keeps yt-dlp on the single video even if the resolved
    // queue item carries a leftover list= param.
    std::wstring yt_cmd = quote(yt) + L" --no-warnings --no-progress "
                                      L"--format bestaudio/best --no-playlist -o - ";
    if (!cfg_.cookies_path.empty())
        yt_cmd += L"--cookies " + quote(cfg_.cookies_path.wstring()) + L" ";
    yt_cmd += L"-- " + quote(widen(play_url));

    std::wstring ff_cmd = quote(ff) + L" -loglevel error -i pipe:0 -f s16le "
                                      L"-acodec pcm_s16le -ar 48000 -ac 2 pipe:1";

    std::wstring tl_cmd = quote(yt) + L" --skip-download --no-warnings --no-playlist "
                                      L"--encoding UTF-8 "
                                      L"--print \"%(title)s\" "
                                      L"--print \"%(uploader)s\" "
                                      L"--print \"%(duration)s\" ";
    if (!cfg_.cookies_path.empty())
        tl_cmd += L"--cookies " + quote(cfg_.cookies_path.wstring()) + L" ";
    tl_cmd += L"-- " + quote(widen(play_url));

    pipe->proc_yt     = spawn_in_job(pipe->job, yt_cmd, nul_in, yt_out_w, err_log);
    const DWORD ec_yt = pipe->proc_yt ? 0u : GetLastError();
    CloseHandle(yt_out_w);
    yt_out_w = nullptr;
    if (!pipe->proc_yt) {
        log::warn("[yt] failed to launch yt-dlp -- {}",
                  describe_launch_failure(std::wstring{yt}, ec_yt, !cfg_.yt_dlp_path.empty()));
        bail();
        if (nul_in) CloseHandle(nul_in);
        if (err_log) CloseHandle(err_log);
        return;
    }

    pipe->proc_ff     = spawn_in_job(pipe->job, ff_cmd, yt_out_r, ff_out_w, err_log);
    const DWORD ec_ff = pipe->proc_ff ? 0u : GetLastError();
    CloseHandle(yt_out_r);
    yt_out_r = nullptr;
    CloseHandle(ff_out_w);
    ff_out_w = nullptr;
    if (!pipe->proc_ff) {
        log::warn("[yt] failed to launch ffmpeg -- {}",
                  describe_launch_failure(std::wstring{ff}, ec_ff, !cfg_.ffmpeg_path.empty()));
        if (ff_out_r) CloseHandle(ff_out_r);
        if (tl_out_r) CloseHandle(tl_out_r);
        if (tl_out_w) CloseHandle(tl_out_w);
        if (nul_in) CloseHandle(nul_in);
        if (err_log) CloseHandle(err_log);
        return; // ~Pipe closes the job, which kills the orphan yt-dlp
    }

    pipe->proc_title = spawn_in_job(pipe->job, tl_cmd, nul_in, tl_out_w, err_log);
    CloseHandle(tl_out_w);
    tl_out_w = nullptr;
    if (pipe->proc_title) {
        pipe->title_pipe = tl_out_r;
        tl_out_r         = nullptr;
    } else if (tl_out_r) {
        CloseHandle(tl_out_r);
        tl_out_r = nullptr;
    }

    if (nul_in) CloseHandle(nul_in);
    if (err_log) CloseHandle(err_log);

    pipe->read_pipe = ff_out_r;
    pipe_           = std::move(pipe);

    info_             = TrackInfo{};
    info_.title       = "(loading)";
    info_.artist      = "YouTube Music";
    info_.duration_ms = 0;
    position_ms_.store(0, std::memory_order_release);
    state_.store(PlaybackState::buffering, std::memory_order_release);

    log::info("[yt] pipe started for {} (track {}/{}; child stderr -> {})", play_url,
              queue_idx_ + 1, queue_.size(), stderr_log_path().string());
}

void YouTubeMusicSource::stop_pipe_locked() {
    pipe_.reset();
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

void YouTubeMusicSource::play() {
    std::scoped_lock lk{mu_};
    if (!pipe_) start_pipe_locked();
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
}

void YouTubeMusicSource::pause() { state_.store(PlaybackState::paused, std::memory_order_release); }

void YouTubeMusicSource::stop() {
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
}

void YouTubeMusicSource::next() {
    std::scoped_lock lk{mu_};
    if (queue_.empty()) return;
    consecutive_failed_ = 0; // user override clears the give-up state
    const auto n        = static_cast<std::ptrdiff_t>(queue_.size());
    auto i              = static_cast<std::ptrdiff_t>(queue_idx_) + 1;
    queue_idx_          = static_cast<std::size_t>(((i % n) + n) % n);
    start_pipe_locked();
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
}

void YouTubeMusicSource::previous() {
    std::scoped_lock lk{mu_};
    if (queue_.empty()) return;
    consecutive_failed_ = 0;
    const auto n        = static_cast<std::ptrdiff_t>(queue_.size());
    auto i              = static_cast<std::ptrdiff_t>(queue_idx_) - 1;
    queue_idx_          = static_cast<std::size_t>(((i % n) + n) % n);
    start_pipe_locked();
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
}

TrackInfo YouTubeMusicSource::current_track() const {
    std::scoped_lock lk{mu_};
    TrackInfo t   = info_;
    t.position_ms = position_ms_.load(std::memory_order_acquire);
    return t;
}

std::string YouTubeMusicSource::auth_instructions() const {
    {
        std::scoped_lock lk{mu_};
        if (!auth_message_.empty()) return auth_message_;
    }
    return "Export your YouTube cookies to a Netscape cookies.txt and set "
           "[youtube_music].cookies_path in config.toml. Public content works "
           "without cookies.";
}

void YouTubeMusicSource::pump(RingBuffer& ring) {
    {
        auto st = state_.load(std::memory_order_acquire);
        if (st != PlaybackState::playing && st != PlaybackState::buffering) return;
    }

    std::scoped_lock lk{mu_};
    Pipe* p = pipe_.get();
    if (!p) return;

    // ---- Title resolver drain & parse ----
    // The earlier version only parsed when the child had exited AND tavail==0
    // in the same tick. In practice yt-dlp --print closes its stdout last; the
    // very next Peek goes ERROR_BROKEN_PIPE and we used to throw the buffered
    // "title\nuploader\nduration\n" away. Now we finalise on broken-pipe too.
    if (p->title_pipe) {
        bool finalise = false;
        for (int safety = 0; safety < 8; ++safety) {
            DWORD tavail = 0;
            BOOL ok      = PeekNamedPipe(p->title_pipe, nullptr, 0, nullptr, &tavail, nullptr);
            if (!ok) {
                finalise = true;
                break;
            }
            if (tavail == 0) {
                DWORD ec = STILL_ACTIVE;
                if (p->proc_title && GetExitCodeProcess(p->proc_title, &ec) && ec != STILL_ACTIVE)
                    finalise = true;
                break;
            }
            char tbuf[1024];
            DWORD got = 0;
            if (!ReadFile(p->title_pipe, tbuf, sizeof(tbuf), &got, nullptr) || got == 0) {
                finalise = true;
                break;
            }
            p->title_buf.append(tbuf, got);
        }
        if (finalise) {
            auto& s        = p->title_buf;
            auto take_line = [&] {
                auto nl          = s.find('\n');
                std::string line = (nl == std::string::npos) ? s : s.substr(0, nl);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                s.erase(0, nl == std::string::npos ? s.size() : nl + 1);
                return line;
            };
            auto title    = take_line();
            auto uploader = take_line();
            auto duration = take_line();
            if (!title.empty() && title != "NA") info_.title = std::move(title);
            if (!uploader.empty() && uploader != "NA") info_.artist = std::move(uploader);
            try {
                if (!duration.empty() && duration != "NA")
                    info_.duration_ms = static_cast<std::uint64_t>(std::stod(duration) * 1000.0);
            } catch (...) {}
            CloseHandle(p->title_pipe);
            p->title_pipe = nullptr;
        }
    }

    // ---- PCM drain ----
    auto advance_to_next = [&] {
        if (queue_.empty()) {
            stop_pipe_locked();
            return;
        }
        const auto n = static_cast<std::ptrdiff_t>(queue_.size());
        auto i       = static_cast<std::ptrdiff_t>(queue_idx_) + 1;
        queue_idx_   = static_cast<std::size_t>(((i % n) + n) % n);
        start_pipe_locked();
        if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
    };

    auto update_position = [&] {
        const std::size_t r        = ring.readable();
        const std::uint64_t played = p->bytes_written > r ? p->bytes_written - r : 0;
        position_ms_.store(played * 1000ull / kPcmBytesPerSec, std::memory_order_release);
    };

    if (p->ended) {
        update_position();
        if (ring.readable() == 0) advance_to_next();
        return;
    }

    if (!p->read_pipe) return;

    auto on_eof = [&] {
        if (p->bytes_written == 0) {
            if (++consecutive_failed_ >= 3) {
                log::warn("[yt] giving up after {} consecutive empty tracks", consecutive_failed_);
                stop_pipe_locked();
                return;
            }
            advance_to_next();
            return;
        }

        consecutive_failed_ = 0;
        p->ended            = true;
        if (p->read_pipe) {
            CloseHandle(p->read_pipe);
            p->read_pipe = nullptr;
        }
    };

    DWORD avail = 0;
    if (!PeekNamedPipe(p->read_pipe, nullptr, 0, nullptr, &avail, nullptr)) {
        on_eof();
        return;
    }
    while (avail > 0) {
        const std::size_t writable = ring.writable();
        if (writable < 4) break;
        std::size_t want = std::min<std::size_t>(writable, avail);
        if (want > 4096) want = 4096;
        std::byte buf[4096];
        DWORD got = 0;
        if (!ReadFile(p->read_pipe, buf, (DWORD)want, &got, nullptr) || got == 0) {
            on_eof();
            return;
        }
        ring.write(buf, got);
        p->bytes_written += got;
        update_position();
        avail = avail > got ? avail - got : 0;
        if (state_.load(std::memory_order_acquire) == PlaybackState::buffering &&
            ring.readable() > 32 * 1024)
            state_.store(PlaybackState::playing, std::memory_order_release);
    }
    // Even when the read loop didn't run (e.g. ring was full), keep position
    // moving as the mixer drains the ring.
    update_position();
}

} // namespace fh6::sources
