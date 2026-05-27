#include "youtube_music_support.hpp"

#include <format>

namespace fh6::sources {

namespace youtube_music_detail {

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
    int n =
        WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::string describe_launch_failure(const std::wstring& bin, DWORD ec, bool from_config) {
    // Resolve where (if anywhere) the binary actually lives. ".exe" default
    // matches what CreateProcess does when no extension is given.
    wchar_t resolved[MAX_PATH] = {};
    DWORD got = SearchPathW(nullptr, bin.c_str(), L".exe", MAX_PATH, resolved, nullptr);
    std::string where =
        got ? narrow({resolved, got})
            : (from_config ? "(configured path not found on disk)" : "(not found on PATH)");

    // FormatMessage gives the localised Win32 string; trim trailing CRLF.
    std::string sys_msg;
    LPWSTR raw = nullptr;
    DWORD len  = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, ec, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&raw, 0, nullptr);
    if (raw && len) {
        while (len && (raw[len - 1] == L'\r' || raw[len - 1] == L'\n' || raw[len - 1] == L' '))
            --len;
        sys_msg = narrow({raw, len});
    }
    if (raw) LocalFree(raw);

    const char* hint = "";
    switch (ec) {
        case ERROR_FILE_NOT_FOUND: // 2
        case ERROR_PATH_NOT_FOUND: // 3
            hint = from_config
                     ? " -- the path in [youtube_music].yt_dlp_path/ffmpeg_path does not "
                       "exist. Fix the path or clear it to fall back to PATH lookup."
                     : " -- yt-dlp/ffmpeg is not on your PATH. Install it (winget install "
                       "yt-dlp.yt-dlp / Gyan.FFmpeg) or set [youtube_music].yt_dlp_path and "
                       "ffmpeg_path in config.toml to the full .exe paths.";
            break;
        case ERROR_ACCESS_DENIED: // 5
            hint = " -- likely blocked or quarantined by antivirus. Whitelist the binary and "
                   "the game folder.";
            break;
        case ERROR_BAD_EXE_FORMAT: // 193
            hint = " -- the file isn't a valid Win64 executable. Download yt-dlp.exe (Windows "
                   "build), not yt-dlp_linux or the bare Python script.";
            break;
        case ERROR_SHARING_VIOLATION: // 32
            hint = " -- another process has the file open (often AV scanning). Retry in a few "
                   "seconds.";
            break;
        default: break;
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

} // namespace youtube_music_detail

YouTubeMusicSource::Pipe::~Pipe() {
    // Close pipes first so any blocked ReadFile in the children unblocks
    // with broken-pipe, then drop the job handle -- KILL_ON_JOB_CLOSE
    // reaps the entire tree (yt-dlp + deno + ffmpeg + title resolver).
    if (read_pipe) CloseHandle(read_pipe);
    if (title_pipe) CloseHandle(title_pipe);
    if (job) CloseHandle(job);
    if (proc_yt) CloseHandle(proc_yt);
    if (proc_ff) CloseHandle(proc_ff);
    if (proc_title) CloseHandle(proc_title);
}

} // namespace fh6::sources
