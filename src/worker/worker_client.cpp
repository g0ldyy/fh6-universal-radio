#include "fh6/worker/worker_client.hpp"
#include "fh6/worker/ipc_protocol.hpp"
#include "fh6/log.hpp"
#include "fh6/subprocess.hpp"

#include <nlohmann/json.hpp>
#include <map>
#include <bcrypt.h>

namespace fh6::worker {

using json = nlohmann::json;

namespace {

// 128-bit cryptographically-random token, lower-case hex. Makes the per-session
// pipe names unguessable so a local process can't squat or inject commands.
std::wstring make_session_token() {
    unsigned char raw[16];
    if (BCryptGenRandom(nullptr, raw, sizeof(raw), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return {};
    static const wchar_t* hex = L"0123456789abcdef";
    std::wstring out;
    out.reserve(sizeof(raw) * 2);
    for (unsigned char b : raw) {
        out += hex[b >> 4];
        out += hex[b & 0x0F];
    }
    return out;
}


// helper to construct a custom environment block for CreateProcessW
std::vector<wchar_t> build_environment_block(const std::vector<std::pair<std::wstring, std::wstring>>& overrides) {
    // case-insensitive map for Windows environment variables
    struct WStringCmpI {
        bool operator()(const std::wstring& a, const std::wstring& b) const {
            return _wcsicmp(a.c_str(), b.c_str()) < 0;
        }
    };
    std::map<std::wstring, std::wstring, WStringCmpI> env_map;

    // fetch current process environment
    LPWCH curr_env = GetEnvironmentStringsW();
    if (!curr_env) return {};
    
    for (LPWCH p = curr_env; *p; ) {
        std::wstring entry(p);
        p += entry.length() + 1;

        if (entry.empty()) continue;

        // start searching for '=' after index 0 to correctly parse variables
        size_t pos = entry.find(L'=', entry[0] == L'=' ? 1 : 0);
        
        if (pos != std::wstring::npos) {
            env_map[entry.substr(0, pos)] = entry.substr(pos + 1);
        }
    }
    FreeEnvironmentStringsW(curr_env);

    // apply overrides
    for (const auto& kv : overrides) {
        if (kv.first.empty() || kv.first.find(L'=') != std::wstring::npos ||
            kv.first.find(L'\0') != std::wstring::npos ||
            kv.second.find(L'\0') != std::wstring::npos) {
            log::warn("[worker] ignoring invalid environment override");
            continue;
        }
        env_map[kv.first] = kv.second;
    }

    // flatten map into a contiguous buffer of null-terminated strings
    std::vector<wchar_t> block;
    for (const auto& kv : env_map) {
        std::wstring entry = kv.first + L"=" + kv.second;
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0'); // final terminator
    return block;
}

// Block until an instance of `name` is available, or the deadline passes.
bool wait_pipe_ready(const std::wstring& name, DWORD total_ms) {
    const ULONGLONG deadline = GetTickCount64() + total_ms;
    do {
        if (WaitNamedPipeW(name.c_str(), 50)) return true;
        Sleep(20); // pipe not created yet (ERROR_FILE_NOT_FOUND) -- back off
    } while (GetTickCount64() < deadline);
    return false;
}

// Connect to a data-stream pipe the worker has already created.
HANDLE connect_to_stream(const std::wstring& pipe_name) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        HANDLE h = CreateFileW(pipe_name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0,
                               nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
        if (GetLastError() == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(pipe_name.c_str(), 200);
        } else {
            Sleep(50);
        }
    }
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

WorkerClient::~WorkerClient() { stop(); }

bool WorkerClient::start(const std::filesystem::path& worker_exe, 
                         const std::vector<std::pair<std::wstring, std::wstring>>& env_overrides) {
    std::scoped_lock lk{mu_};
    if (process_) return true; // already started

    if (!std::filesystem::exists(worker_exe)) {
        log::warn("[worker] worker exe not found at {}", worker_exe.string());
        return false;
    }

    token_ = make_session_token();
    if (token_.empty()) {
        log::error("[worker] failed to generate session token");
        return false;
    }

    // Launch the worker. This is the ONE fork() from the game process. We hand
    // it the token (for the pipe names) and our PID, so it self-terminates --
    // taking its children with it -- if the game ever dies without a clean stop.
    std::wstring cmd = subprocess::quote(worker_exe.wstring()) + L" " + token_ + L" " +
                       std::to_wstring(GetCurrentProcessId());

    // build the custom environment block
    std::vector<wchar_t> env_block = build_environment_block(env_overrides);
    if (env_block.size() <= 2) {
        log::error("[worker] failed to read inherited environment");
        token_.clear();
        return false;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // add CREATE_UNICODE_ENVIRONMENT to the creation flags
    DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
    
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, flags, env_block.data(),
                        subprocess::safe_spawn_cwd(), &si, &pi)) {
        log::error("[worker] failed to launch worker (err {})", GetLastError());
        token_.clear();
        return false;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;

    // Confirm the worker came up by waiting for its control pipe to appear.
    if (!wait_pipe_ready(control_pipe_name(token_), 5000)) {
        log::error("[worker] control pipe never appeared");
        TerminateProcess(process_, 1);
        CloseHandle(process_);
        process_ = nullptr;
        token_.clear();
        return false;
    }

    log::info("[worker] connected to worker process (pid {})", pi.dwProcessId);
    return true;
}

void WorkerClient::stop() {
    std::scoped_lock lk{mu_};
    if (!process_) return;

    if (!token_.empty()) request(R"({"op":"shutdown"})"); // best-effort graceful exit
    if (WaitForSingleObject(process_, 3000) == WAIT_TIMEOUT)
        TerminateProcess(process_, 1);
    CloseHandle(process_);
    process_ = nullptr;
    // token_ is left set on purpose: request() reads it lock-free from source
    // threads that may still be tearing down, so clearing it would race them. A
    // stale token just fails to connect to the now-dead worker, which is fine.
}

bool WorkerClient::alive() const noexcept {
    std::scoped_lock lk{mu_};
    if (!process_) return false;
    DWORD ec = 0;
    return GetExitCodeProcess(process_, &ec) && ec == STILL_ACTIVE;
}

// ---------------------------------------------------------------------------
// Control transport: one connection per request (lock-free, fully concurrent)
// ---------------------------------------------------------------------------

std::string WorkerClient::request(const std::string& req, bool want_response) const {
    if (token_.empty()) return {};
    const std::wstring name = control_pipe_name(token_);

    HANDLE h = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 50 && h == INVALID_HANDLE_VALUE; ++attempt) {
        h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0,
                        nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY) break; // worker gone, not just busy
        WaitNamedPipeW(name.c_str(), 200);             // all instances busy -- wait for one
    }
    if (h == INVALID_HANDLE_VALUE) {
        log::warn("[worker] could not open control pipe (worker down?)");
        return {};
    }

    std::string resp;
    if (ipc_send(h, req) && want_response) resp = ipc_recv(h);
    CloseHandle(h);
    return resp;
}

// ---------------------------------------------------------------------------
// Synchronous run + capture
// ---------------------------------------------------------------------------

std::string WorkerClient::run_capture(const std::wstring& cmd, bool capture_stderr) {
    json req = {{"op", "run"}, {"cmd", subprocess::narrow(cmd)}, {"capture_stderr", capture_stderr}};
    auto resp_str = request(req.dump());
    if (resp_str.empty()) return {};

    try {
        auto resp = json::parse(resp_str);
        if (resp.value("ok", false)) return resp.value("output", "");
        log::warn("[worker] run failed: {}", resp.value("error", "unknown"));
    } catch (...) {
        log::warn("[worker] malformed run response");
    }
    return {};
}

// ---------------------------------------------------------------------------
// Asynchronous pipeline spawn
// ---------------------------------------------------------------------------

WorkerClient::SpawnResult WorkerClient::spawn_pipeline(const std::vector<std::wstring>& chain,
                                                       const std::wstring& side_cmd,
                                                       bool capture_stderr_meta,
                                                       int meta_stderr_idx,
                                                       uint32_t out_buffer_size) {
    SpawnResult out;
    if (token_.empty()) return out;

    out.pipeline_id = next_id_.fetch_add(1, std::memory_order_relaxed);

    json cmds = json::array();
    for (const auto& c : chain) cmds.push_back(subprocess::narrow(c));

    json req = {{"op", "spawn"}, {"id", out.pipeline_id}, {"chain", cmds}};
    if (!side_cmd.empty()) req["side_cmd"] = subprocess::narrow(side_cmd);
    
    if (capture_stderr_meta) req["capture_stderr_meta"] = true;
    if (meta_stderr_idx >= 0) req["meta_stderr_idx"] = meta_stderr_idx;
    if (out_buffer_size > 0) req["out_buffer_size"] = out_buffer_size;

    auto resp_str = request(req.dump());
    if (resp_str.empty()) return out;

    try {
        auto resp = json::parse(resp_str);
        if (!resp.value("ok", false)) {
            log::warn("[worker] spawn failed: {}", resp.value("error", "unknown"));
            return out;
        }
        // Connect to the data streams the worker created.
        if (resp.contains("pcm_pipe")) {
            out.pcm_pipe = connect_to_stream(subprocess::widen(resp["pcm_pipe"].get<std::string>()));
        }
        if (resp.contains("meta_pipe")) {
            out.meta_pipe =
                connect_to_stream(subprocess::widen(resp["meta_pipe"].get<std::string>()));
        }
        out.ok = (out.pcm_pipe != nullptr);
    } catch (...) {
        log::warn("[worker] malformed spawn response");
    }

    // If we couldn't attach to the PCM stream, the worker still holds a live
    // pipeline (child processes + proxy threads). Tell it to tear down, else it
    // leaks the very processes this worker exists to reap.
    if (!out.ok) {
        if (out.meta_pipe) {
            CloseHandle(out.meta_pipe);
            out.meta_pipe = nullptr;
        }
        kill_pipeline(out.pipeline_id);
    }
    return out;
}

WorkerClient::SpawnResult WorkerClient::spawn_single(const std::wstring& cmd) {
    return spawn_pipeline({cmd});
}

void WorkerClient::kill_pipeline(uint32_t id) {
    // Fire-and-forget: sources call this from ~Pipe under their audio lock, and
    // the teardown (reaping a yt-dlp tree, joining proxy threads) can take tens
    // of ms -- waiting for it would reintroduce the stutter the worker removes.
    // The worker reaps on its own; an unread reply is harmless.
    request(json({{"op", "kill"}, {"id", id}}).dump(), /*want_response=*/false);
}

} // namespace fh6::worker
