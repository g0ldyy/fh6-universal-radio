#include "fh6/sources/youtube_music_source.hpp"
#include "fh6/log.hpp"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fh6::sources {

namespace {

// PCM contract written by ffmpeg: 48000 Hz * 2 ch * 2 bytes.
constexpr std::uint64_t kPcmBytesPerSec = 48000ull * 2ull * 2ull;

// CreateProcess hands one string to the child via GetCommandLineW, so any
// argument with whitespace must be double-quoted.
std::wstring quote(const std::wstring& s) {
    if (s.empty()) return L"\"\"";
    if (s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    std::wstring out{L"\""};
    for (auto c : s) {
        if (c == L'"') out += L'\\';
        out += c;
    }
    out += L'"';
    return out;
}

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

// GetStdHandle returns NULL in a windowed DLL injection; passing NULL with
// STARTF_USESTDHANDLES makes the child's stdio invalid and yt-dlp exits
// before producing audio. NUL is Windows' /dev/null and works as a safe substitute.
HANDLE open_nul(DWORD access) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE h = CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                           0, nullptr);
    return h == INVALID_HANDLE_VALUE ? nullptr : h;
}

// Tee both children's stderr to %TEMP%\fh6-yt-stderr.log so failures (bad
// cookies, geo-block, codec issues) can be diagnosed without a debug build.
// FILE_APPEND_DATA makes per-syscall writes atomic across all children.
HANDLE open_stderr_log() {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    auto path = std::filesystem::temp_directory_path() / "fh6-yt-stderr.log";
    HANDLE h  = CreateFileW(path.wstring().c_str(), FILE_APPEND_DATA | SYNCHRONIZE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    return h == INVALID_HANDLE_VALUE ? open_nul(GENERIC_WRITE) : h;
}

std::filesystem::path stderr_log_path() {
    return std::filesystem::temp_directory_path() / "fh6-yt-stderr.log";
}

std::string narrow(std::wstring_view ws) {
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), nullptr, 0,
                                nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::string describe_launch_failure(const std::wstring& bin, DWORD ec, bool from_config) {
    // Resolve where (if anywhere) the binary actually lives. ".exe" default
    // matches what CreateProcess does when no extension is given.
    wchar_t resolved[MAX_PATH] = {};
    DWORD got = SearchPathW(nullptr, bin.c_str(), L".exe", MAX_PATH, resolved, nullptr);
    std::string where = got ? narrow({resolved, got})
                            : (from_config ? "(configured path not found on disk)"
                                           : "(not found on PATH)");

    // FormatMessage gives the localised Win32 string; trim trailing CRLF.
    std::string sys_msg;
    LPWSTR raw = nullptr;
    DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                   FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, ec, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                               (LPWSTR)&raw, 0, nullptr);
    if (raw && len) {
        while (len && (raw[len - 1] == L'\r' || raw[len - 1] == L'\n' || raw[len - 1] == L' '))
            --len;
        sys_msg = narrow({raw, len});
    }
    if (raw) LocalFree(raw);

    const char* hint = "";
    switch (ec) {
        case ERROR_FILE_NOT_FOUND:  // 2
        case ERROR_PATH_NOT_FOUND:  // 3
            hint = from_config
                       ? " -- the path in [youtube_music].yt_dlp_path/ffmpeg_path does not "
                         "exist. Fix the path or clear it to fall back to PATH lookup."
                       : " -- yt-dlp/ffmpeg is not on your PATH. Install it (winget install "
                         "yt-dlp.yt-dlp / Gyan.FFmpeg) or set [youtube_music].yt_dlp_path and "
                         "ffmpeg_path in config.toml to the full .exe paths.";
            break;
        case ERROR_ACCESS_DENIED:  // 5
            hint = " -- likely blocked or quarantined by antivirus. Whitelist the binary and "
                   "the game folder.";
            break;
        case ERROR_BAD_EXE_FORMAT:  // 193
            hint = " -- the file isn't a valid Win64 executable. Download yt-dlp.exe (Windows "
                   "build), not yt-dlp_linux or the bare Python script.";
            break;
        case ERROR_SHARING_VIOLATION:  // 32
            hint = " -- another process has the file open (often AV scanning). Retry in a few "
                   "seconds.";
            break;
        default:
            break;
    }

    return std::format("ec={} ({}) tried={} resolved={}{}", ec,
                       sys_msg.empty() ? "unknown" : sys_msg, narrow(bin), where, hint);
}

// Job Object with KILL_ON_JOB_CLOSE so closing the last handle reaps every
// assigned process AND its descendants. yt-dlp spawns deno (the JS runtime
// it needs to solve YouTube's n-challenge); without a job, terminating
// yt-dlp leaves deno orphaned, and that's why ps showed yt-dlp/deno still
// alive after FH6 exited. Forza exiting closes our DLL's handles, which
// drops the job's last ref, which kills everything inside.
HANDLE create_kill_on_close_job() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

// Spawn under `job`. CREATE_SUSPENDED + AssignProcessToJobObject + ResumeThread
// ensures yt-dlp is inside the job before it can spawn deno -- otherwise a
// fast-starting child could escape into its own process tree.
HANDLE spawn_in_job(HANDLE job, const std::wstring& cmd, HANDLE stdin_h, HANDLE stdout_h,
                    HANDLE stderr_h) {
    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = stdin_h;
    si.hStdOutput = stdout_h;
    si.hStdError  = stderr_h;

    PROCESS_INFORMATION pi{};
    std::wstring mut = cmd;
    if (!CreateProcessW(nullptr, mut.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi))
        return nullptr;
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
        // Preserve the AssignProcessToJobObject error across the cleanup calls so
        // the caller's GetLastError() reflects the real cause, not CloseHandle's.
        const DWORD assign_ec = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        SetLastError(assign_ec);
        return nullptr;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

// Block until the child closes its stdout. Used by the playlist enumerator,
// which is small (one id per line) and runs once per cast.
std::string drain_to_eof(HANDLE pipe) {
    std::string out;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(pipe, buf, sizeof(buf), &got, nullptr) && got > 0) out.append(buf, got);
    return out;
}

bool is_playlist_url(std::string_view url) {
    return url.find("playlist?") != std::string_view::npos;
}

std::string watch_url_for_id(std::string_view id) {
    std::string s = "https://www.youtube.com/watch?v=";
    s.append(id);
    return s;
}

} // namespace

struct YouTubeMusicSource::Pipe {
    HANDLE job        = nullptr;
    HANDLE proc_yt    = nullptr;
    HANDLE proc_ff    = nullptr;
    HANDLE proc_title = nullptr;
    HANDLE read_pipe  = nullptr;
    HANDLE title_pipe = nullptr;
    std::string title_buf;
    std::uint64_t bytes_written = 0;
    bool ended = false;

    ~Pipe() {
        // Close pipes first so any blocked ReadFile in the children unblocks
        // with broken-pipe, then drop the job handle -- KILL_ON_JOB_CLOSE
        // reaps the entire tree (yt-dlp + deno + ffmpeg + title resolver).
        if (read_pipe)  CloseHandle(read_pipe);
        if (title_pipe) CloseHandle(title_pipe);
        if (job)        CloseHandle(job);
        if (proc_yt)    CloseHandle(proc_yt);
        if (proc_ff)    CloseHandle(proc_ff);
        if (proc_title) CloseHandle(proc_title);
    }
};

YouTubeMusicSource::YouTubeMusicSource(YouTubeMusicConfig cfg) : cfg_{std::move(cfg)} {}

YouTubeMusicSource::~YouTubeMusicSource() { stop_pipe_locked(); }

bool YouTubeMusicSource::initialize() {
    if (!cfg_.enabled) return false;
    if (!cfg_.default_playlist.empty()) {
        std::scoped_lock lk{mu_};
        target_url_ = cfg_.default_playlist;
    }
    auth_ = cfg_.cookies_path.empty() ? AuthState::none_required : AuthState::authenticated;
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
    if (nul_in)  CloseHandle(nul_in);
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
    CloseHandle(job);  // KILL_ON_JOB_CLOSE -- ensures any straggling deno child dies

    for (std::size_t pos = 0; pos < raw.size();) {
        auto nl   = raw.find('\n', pos);
        auto end  = (nl == std::string::npos) ? raw.size() : nl;
        auto line = raw.substr(pos, end - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        if (!line.empty() && line != "NA") queue_.push_back(watch_url_for_id(line));
        pos = (nl == std::string::npos) ? raw.size() : nl + 1;
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

    if (!CreatePipe(&yt_out_r, &yt_out_w, &sa, 1 << 20)) { bail(); return; }
    SetHandleInformation(yt_out_r, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    if (!CreatePipe(&ff_out_r, &ff_out_w, &sa, 1 << 20)) { bail(); return; }
    SetHandleInformation(ff_out_r, 0, HANDLE_FLAG_INHERIT);
    if (!CreatePipe(&tl_out_r, &tl_out_w, &sa, 1 << 16)) { bail(); return; }
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

    pipe->proc_yt = spawn_in_job(pipe->job, yt_cmd, nul_in, yt_out_w, err_log);
    const DWORD ec_yt = pipe->proc_yt ? 0u : GetLastError();
    CloseHandle(yt_out_w);
    yt_out_w = nullptr;
    if (!pipe->proc_yt) {
        log::warn("[yt] failed to launch yt-dlp -- {}",
                  describe_launch_failure(std::wstring{yt}, ec_yt, !cfg_.yt_dlp_path.empty()));
        bail();
        if (nul_in)  CloseHandle(nul_in);
        if (err_log) CloseHandle(err_log);
        return;
    }

    pipe->proc_ff = spawn_in_job(pipe->job, ff_cmd, yt_out_r, ff_out_w, err_log);
    const DWORD ec_ff = pipe->proc_ff ? 0u : GetLastError();
    CloseHandle(yt_out_r); yt_out_r = nullptr;
    CloseHandle(ff_out_w); ff_out_w = nullptr;
    if (!pipe->proc_ff) {
        log::warn("[yt] failed to launch ffmpeg -- {}",
                  describe_launch_failure(std::wstring{ff}, ec_ff, !cfg_.ffmpeg_path.empty()));
        if (ff_out_r) CloseHandle(ff_out_r);
        if (tl_out_r) CloseHandle(tl_out_r);
        if (tl_out_w) CloseHandle(tl_out_w);
        if (nul_in)   CloseHandle(nul_in);
        if (err_log)  CloseHandle(err_log);
        return;  // ~Pipe closes the job, which kills the orphan yt-dlp
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

    if (nul_in)  CloseHandle(nul_in);
    if (err_log) CloseHandle(err_log);

    pipe->read_pipe = ff_out_r;
    pipe_           = std::move(pipe);

    info_              = TrackInfo{};
    info_.title        = "(loading)";
    info_.artist       = "YouTube Music";
    info_.duration_ms  = 0;
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

void YouTubeMusicSource::pause() {
    state_.store(PlaybackState::paused, std::memory_order_release);
}

void YouTubeMusicSource::stop() {
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
}

void YouTubeMusicSource::next() {
    std::scoped_lock lk{mu_};
    if (queue_.empty()) return;
    consecutive_failed_ = 0;   // user override clears the give-up state
    const auto n = static_cast<std::ptrdiff_t>(queue_.size());
    auto i       = static_cast<std::ptrdiff_t>(queue_idx_) + 1;
    queue_idx_   = static_cast<std::size_t>(((i % n) + n) % n);
    start_pipe_locked();
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
}

void YouTubeMusicSource::previous() {
    std::scoped_lock lk{mu_};
    if (queue_.empty()) return;
    consecutive_failed_ = 0;
    const auto n = static_cast<std::ptrdiff_t>(queue_.size());
    auto i       = static_cast<std::ptrdiff_t>(queue_idx_) - 1;
    queue_idx_   = static_cast<std::size_t>(((i % n) + n) % n);
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
            if (!ok) { finalise = true; break; }
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
        if (queue_.empty()) { stop_pipe_locked(); return; }
        const auto n = static_cast<std::ptrdiff_t>(queue_.size());
        auto i       = static_cast<std::ptrdiff_t>(queue_idx_) + 1;
        queue_idx_   = static_cast<std::size_t>(((i % n) + n) % n);
        start_pipe_locked();
        if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
    };

    auto update_position = [&] {
        const std::size_t r   = ring.readable();
        const std::uint64_t played =
            p->bytes_written > r ? p->bytes_written - r : 0;
        position_ms_.store(played * 1000ull / kPcmBytesPerSec,
                           std::memory_order_release);
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
                log::warn("[yt] giving up after {} consecutive empty tracks",
                          consecutive_failed_);
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
