#include "fh6/sources/airplay_source.hpp"
#include "fh6/log.hpp"

#include <windows.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>

namespace fh6::sources {

namespace {

// PCM contract written by ffmpeg into the ring: 48000 Hz * 2 ch * 2 bytes.
constexpr std::uint64_t kPcmBytesPerSec = 48000ull * 2ull * 2ull;

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

std::string narrow(std::wstring_view ws) {
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), out.data(), n, nullptr, nullptr);
    return out;
}

// See youtube_music_source.cpp: GetStdHandle is NULL in an injected DLL, so we
// hand children Windows' NUL device instead of an invalid handle.
HANDLE open_nul(DWORD access) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE h = CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                           0, nullptr);
    return h == INVALID_HANDLE_VALUE ? nullptr : h;
}

// Tee both children's stderr to %TEMP%\fh6-airplay-stderr.log so receiver/ffmpeg
// failures (missing binary, no Bonjour, busy port) are diagnosable.
HANDLE open_stderr_log() {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    auto path = std::filesystem::temp_directory_path() / "fh6-airplay-stderr.log";
    HANDLE h  = CreateFileW(path.wstring().c_str(), FILE_APPEND_DATA | SYNCHRONIZE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    return h == INVALID_HANDLE_VALUE ? open_nul(GENERIC_WRITE) : h;
}

std::filesystem::path stderr_log_path() {
    return std::filesystem::temp_directory_path() / "fh6-airplay-stderr.log";
}

std::string describe_launch_failure(const std::wstring& bin, DWORD ec, bool from_config) {
    wchar_t resolved[MAX_PATH] = {};
    DWORD got = SearchPathW(nullptr, bin.c_str(), L".exe", MAX_PATH, resolved, nullptr);
    std::string where = got ? narrow({resolved, got})
                            : (from_config ? "(configured path not found on disk)"
                                           : "(not found on PATH)");

    std::string sys_msg;
    LPWSTR raw = nullptr;
    DWORD len  = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                   FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, ec, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&raw,
                               0, nullptr);
    if (raw && len) {
        while (len && (raw[len - 1] == L'\r' || raw[len - 1] == L'\n' || raw[len - 1] == L' '))
            --len;
        sys_msg = narrow({raw, len});
    }
    if (raw) LocalFree(raw);

    const char* hint = "";
    if (ec == ERROR_FILE_NOT_FOUND || ec == ERROR_PATH_NOT_FOUND) {
        hint = from_config
                   ? " -- the path in [airplay].receiver_path/ffmpeg_path does not exist. Fix the "
                     "path or clear it to fall back to PATH lookup."
                   : " -- the AirPlay receiver or ffmpeg is not on your PATH. Install a receiver "
                     "(e.g. shairport-sync, requires Apple Bonjour for discovery) and ffmpeg, or "
                     "set [airplay].receiver_path and ffmpeg_path in config.toml.";
    }

    return std::format("ec={} ({}) tried={} resolved={}{}", ec,
                       sys_msg.empty() ? "unknown" : sys_msg, narrow(bin), where, hint);
}

// Same KILL_ON_JOB_CLOSE trick the YouTube source uses: closing the job handle
// reaps the receiver, ffmpeg, and any helpers they spawned when FH6 exits.
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

} // namespace

struct AirPlaySource::Pipe {
    HANDLE job          = nullptr;
    HANDLE proc_recv    = nullptr;
    HANDLE proc_ff      = nullptr;
    HANDLE read_pipe    = nullptr;
    std::uint64_t bytes_written = 0;

    ~Pipe() {
        if (read_pipe) CloseHandle(read_pipe);
        if (job)       CloseHandle(job);
        if (proc_recv) CloseHandle(proc_recv);
        if (proc_ff)   CloseHandle(proc_ff);
    }
};

AirPlaySource::AirPlaySource(AirPlayConfig cfg) : cfg_{std::move(cfg)} {}

AirPlaySource::~AirPlaySource() {
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
}

bool AirPlaySource::initialize() { return cfg_.enabled; }

void AirPlaySource::shutdown() noexcept {
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
}

std::string AirPlaySource::auth_instructions() const {
    return "Install an AirPlay receiver (shairport-sync) and ffmpeg, then select this source. "
           "On your phone, open Control Center, tap AirPlay, and pick \"" + cfg_.service_name +
           "\". Play Apple Music (or anything) and it streams into the radio.";
}

void AirPlaySource::start_pipe_locked() {
    stop_pipe_locked();

    auto pipe = std::make_unique<Pipe>();
    pipe->job = create_kill_on_close_job();
    if (!pipe->job) {
        log::warn("[airplay] CreateJobObject failed ({})", GetLastError());
        return;
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rv_out_r = nullptr, rv_out_w = nullptr;  // receiver stdout -> ffmpeg stdin
    HANDLE ff_out_r = nullptr, ff_out_w = nullptr;  // ffmpeg stdout -> us

    auto bail = [&] {
        if (rv_out_r) CloseHandle(rv_out_r);
        if (rv_out_w) CloseHandle(rv_out_w);
        if (ff_out_r) CloseHandle(ff_out_r);
        if (ff_out_w) CloseHandle(ff_out_w);
    };

    if (!CreatePipe(&rv_out_r, &rv_out_w, &sa, 1 << 20)) { bail(); return; }
    SetHandleInformation(rv_out_r, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    if (!CreatePipe(&ff_out_r, &ff_out_w, &sa, 1 << 20)) { bail(); return; }
    SetHandleInformation(ff_out_r, 0, HANDLE_FLAG_INHERIT);

    HANDLE nul_in  = open_nul(GENERIC_READ);
    HANDLE err_log = open_stderr_log();

    const auto recv = cfg_.receiver_path.empty() ? L"shairport-sync" : cfg_.receiver_path.wstring();
    const auto ff   = cfg_.ffmpeg_path.empty() ? L"ffmpeg" : cfg_.ffmpeg_path.wstring();
    const uint32_t in_rate = cfg_.input_sample_rate ? cfg_.input_sample_rate : 44100u;

    // shairport-sync's stdout backend emits raw S16_LE / 2ch at its session
    // rate; `-a` sets the name shown in the phone's AirPlay picker.
    std::wstring rv_cmd = quote(recv) + L" -a " + quote(widen(cfg_.service_name)) + L" -o stdout";

    std::wstring ff_cmd =
        quote(ff) + std::format(L" -loglevel error -f s16le -ar {} -ac 2 -i pipe:0 "
                                L"-f s16le -acodec pcm_s16le -ar 48000 -ac 2 pipe:1",
                                in_rate);

    pipe->proc_recv = spawn_in_job(pipe->job, rv_cmd, nul_in, rv_out_w, err_log);
    const DWORD ec_recv = pipe->proc_recv ? 0u : GetLastError();
    CloseHandle(rv_out_w);
    rv_out_w = nullptr;
    if (!pipe->proc_recv) {
        log::warn("[airplay] failed to launch receiver -- {}",
                  describe_launch_failure(std::wstring{recv}, ec_recv, !cfg_.receiver_path.empty()));
        if (rv_out_r) CloseHandle(rv_out_r);
        if (ff_out_r) CloseHandle(ff_out_r);
        if (ff_out_w) CloseHandle(ff_out_w);
        if (nul_in)   CloseHandle(nul_in);
        if (err_log)  CloseHandle(err_log);
        return;
    }

    pipe->proc_ff = spawn_in_job(pipe->job, ff_cmd, rv_out_r, ff_out_w, err_log);
    const DWORD ec_ff = pipe->proc_ff ? 0u : GetLastError();
    CloseHandle(rv_out_r); rv_out_r = nullptr;
    CloseHandle(ff_out_w); ff_out_w = nullptr;
    if (!pipe->proc_ff) {
        log::warn("[airplay] failed to launch ffmpeg -- {}",
                  describe_launch_failure(std::wstring{ff}, ec_ff, !cfg_.ffmpeg_path.empty()));
        if (ff_out_r) CloseHandle(ff_out_r);
        if (nul_in)   CloseHandle(nul_in);
        if (err_log)  CloseHandle(err_log);
        return;  // ~Pipe closes the job, killing the orphaned receiver
    }

    if (nul_in)  CloseHandle(nul_in);
    if (err_log) CloseHandle(err_log);

    pipe->read_pipe = ff_out_r;
    pipe_           = std::move(pipe);

    position_ms_.store(0, std::memory_order_release);
    state_.store(PlaybackState::buffering, std::memory_order_release);

    log::info("[airplay] receiver '{}' advertised as \"{}\" (child stderr -> {})", narrow(recv),
              cfg_.service_name, stderr_log_path().string());
}

void AirPlaySource::stop_pipe_locked() {
    pipe_.reset();
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

void AirPlaySource::play() {
    std::scoped_lock lk{mu_};
    if (!pipe_) start_pipe_locked();
    if (pipe_ && state_.load(std::memory_order_acquire) == PlaybackState::stopped)
        state_.store(PlaybackState::buffering, std::memory_order_release);
}

// A live network stream can't be paused at the receiver, so pausing tears the
// receiver down; play() re-advertises it. This also lets source switching
// (which pauses the outgoing source) free the AirPlay port cleanly.
void AirPlaySource::pause() {
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
}

void AirPlaySource::stop() {
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
}

TrackInfo AirPlaySource::current_track() const {
    std::scoped_lock lk{mu_};
    TrackInfo t;
    t.title       = "AirPlay";
    t.artist      = cfg_.service_name;
    t.position_ms = position_ms_.load(std::memory_order_acquire);
    return t;
}

void AirPlaySource::pump(RingBuffer& ring) {
    {
        auto st = state_.load(std::memory_order_acquire);
        if (st != PlaybackState::playing && st != PlaybackState::buffering) return;
    }

    std::scoped_lock lk{mu_};
    Pipe* p = pipe_.get();
    if (!p || !p->read_pipe) return;

    auto update_position = [&] {
        const std::size_t r        = ring.readable();
        const std::uint64_t played = p->bytes_written > r ? p->bytes_written - r : 0;
        position_ms_.store(played * 1000ull / kPcmBytesPerSec, std::memory_order_release);
    };

    DWORD avail = 0;
    if (!PeekNamedPipe(p->read_pipe, nullptr, 0, nullptr, &avail, nullptr)) {
        // Broken pipe means the receiver (or ffmpeg) exited. Idle senders just
        // produce no data, so this is a real teardown, not a between-songs gap.
        log::warn("[airplay] receiver stream ended -- check {}", stderr_log_path().string());
        stop_pipe_locked();
        return;
    }

    // No data yet: a phone hasn't connected or is between tracks. Stay buffering.
    if (avail == 0) {
        update_position();
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
            log::warn("[airplay] receiver stream ended -- check {}", stderr_log_path().string());
            stop_pipe_locked();
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
    update_position();
}

} // namespace fh6::sources
