// fh6-radio-worker: lightweight process that spawns yt-dlp / ffmpeg on behalf
// of the game-injected DLL.  Lives outside the multi-GB game address space so
// the fork() that Wine/Proton performs inside CreateProcess is cheap (~5 MB
// instead of several GB), eliminating the in-game stutter that occurs on every
// track change.
//
// Invocation:  fh6-radio-worker.exe <session-token> [<parent-pid>]
//   token       -- embedded in every pipe name so they are unguessable.
//   parent-pid  -- if given, the worker self-terminates (killing its children)
//                  when that process exits, so nothing is orphaned on a crash.
//
// Protocol: length-prefixed JSON; one connection per request on a multi-instance
//           control pipe, so a slow capture never blocks an audio spawn/kill.
// Data streams: per-pipeline named pipes \\.\pipe\fh6-radio-<token>-<id>-pcm/meta.

#include "fh6/worker/ipc_protocol.hpp"
#include "fh6/subprocess.hpp"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <sddl.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;
using namespace fh6::worker;
using namespace fh6::subprocess;

namespace {

std::wstring g_token;                    // session token; set once in main()
SECURITY_ATTRIBUTES* g_pipe_sa = nullptr; // owner-restricted DACL for our pipes

// DACL limiting our pipes to authenticated users + SYSTEM/Admins (never network
// or anonymous). With the random token in the name this blocks squatting and
// injection; on failure the unguessable name alone still protects us.
SECURITY_ATTRIBUTES* make_pipe_sa() {
    static SECURITY_ATTRIBUTES sa{};
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;AU)(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &sd, nullptr))
        return nullptr;
    sa.nLength              = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle       = FALSE;
    return &sa;
}

// ---------------------------------------------------------------------------
// Pipeline: a set of child processes + proxy threads for one audio track.
// ---------------------------------------------------------------------------

// Bridges a child's stdout (anonymous pipe) to the named pipe the DLL reads.
// The Pipeline owns these handles; the proxy only uses them, so kill() can
// close them race-free once it has joined the thread.
struct ProxyData {
    HANDLE source;       // anonymous pipe read-end (child stdout)
    HANDLE dest;         // named pipe server-end (DLL reads the client end)
    std::wstring name;   // dest's pipe name, for the self-connect unblock
    std::atomic<bool> stop{false};
    ProxyData(HANDLE s, HANDLE d, std::wstring n) : source(s), dest(d), name(std::move(n)) {}
};

struct Pipeline {
    uint32_t id = 0;
    HANDLE job  = nullptr;
    std::vector<HANDLE> processes;
    std::vector<std::unique_ptr<ProxyData>> proxies_data;
    std::vector<std::thread> proxies;

    void kill() {
        // 1. Tell the proxies to stop touching their pipes.
        for (auto& pd : proxies_data) pd->stop.store(true, std::memory_order_release);

        // 2. Reap the child trees -- this is what unblocks a proxy parked in
        //    ReadFile(source). yt-dlp (PyInstaller) survives a bare
        //    TerminateProcess, so reap() walks the whole tree (Job Objects don't
        //    reap reliably under Wine).
        for (HANDLE& h : processes) reap(h);
        processes.clear();
        if (job) { CloseHandle(job); job = nullptr; }

        // 3. Unblock any proxy still parked on dest. stop is set, so none will
        //    touch dest again; we only break the connection here (close is step
        //    5, post-join), so there's no use-after-close. Parked on WriteFile ->
        //    DisconnectNamedPipe breaks it; parked on ConnectNamedPipe (DLL never
        //    connected) -> a throwaway client completes it.
        for (auto& pd : proxies_data) {
            DisconnectNamedPipe(pd->dest);
            if (HANDLE c = CreateFileW(pd->name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0,
                                       nullptr);
                c != INVALID_HANDLE_VALUE)
                CloseHandle(c);
        }

        // 4. Join -- after this we are the sole owner of every proxy handle.
        for (auto& t : proxies)
            if (t.joinable()) t.join();
        proxies.clear();

        // 5. Close the pipes (no races now: the proxies have all exited).
        for (auto& pd : proxies_data) {
            DisconnectNamedPipe(pd->dest);
            CloseHandle(pd->dest);
            CloseHandle(pd->source);
        }
        proxies_data.clear();
    }

    ~Pipeline() { kill(); }
};

std::mutex g_mu;
std::unordered_map<uint32_t, std::unique_ptr<Pipeline>> g_pipelines;

// Kill every child tree, then terminate the worker. Called on an explicit
// shutdown op and on parent-process death.
[[noreturn]] void shutdown_and_exit(UINT code) {
    {
        std::scoped_lock lk{g_mu};
        g_pipelines.clear(); // Pipeline dtors terminate child trees + join proxies
    }
    ExitProcess(code);
}

// ---------------------------------------------------------------------------
// Proxy thread: pump one child's stdout to its named data pipe.
// ---------------------------------------------------------------------------

void proxy_thread_fn(ProxyData* d) {
    // Wait for the DLL to connect its client end (returns immediately if it
    // already has, or once kill()'s self-connect satisfies it).
    if (!d->stop.load(std::memory_order_acquire))
        ConnectNamedPipe(d->dest, nullptr);

    char buf[8192];
    while (!d->stop.load(std::memory_order_acquire)) {
        DWORD got = 0;
        if (!ReadFile(d->source, buf, sizeof(buf), &got, nullptr) || got == 0) break;
        DWORD written = 0;
        if (!WriteFile(d->dest, buf, got, &written, nullptr)) break;
    }

    // Graceful child EOF: flush (so Wine doesn't drop undrained bytes), then
    // disconnect so the DLL's reader sees end-of-track. dest is closed by kill()
    // after the join, never here, so the two never race on it. On kill (stop set)
    // the client is already gone -- leave dest entirely to kill().
    if (!d->stop.load(std::memory_order_acquire)) {
        FlushFileBuffers(d->dest);
        DisconnectNamedPipe(d->dest);
    }
}

HANDLE create_stream_pipe(const std::wstring& name, DWORD out_buffer_size = 1 << 20) {
    return CreateNamedPipeW(name.c_str(), PIPE_ACCESS_OUTBOUND, // server writes, client reads
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                            1,                 // max instances
                            out_buffer_size,   // dynamic out buffer
                            0,                 // in buffer (unused for outbound)
                            0,                 // default timeout
                            g_pipe_sa);
}

// ---------------------------------------------------------------------------
// Handle "run" -- run a command and return its captured output. An empty
// output also signals failure to the DLL, which then falls back to a direct
// spawn, so no separate error channel is needed.
// ---------------------------------------------------------------------------

json handle_run(const json& req) {
    const std::wstring cmd    = widen(req.at("cmd").get<std::string>());
    const bool capture_stderr = req.value("capture_stderr", false);
    return {{"ok", true}, {"output", capture_output(cmd, capture_stderr)}};
}

// ---------------------------------------------------------------------------
// Handle "spawn" -- asynchronous pipeline with named-pipe data streams.
// ---------------------------------------------------------------------------

json handle_spawn(const json& req) {
    uint32_t id = req.at("id").get<uint32_t>();
    auto chain  = req.at("chain").get<std::vector<std::string>>();
    if (chain.empty()) return {{"ok", false}, {"error", "empty chain"}};

    bool capture_stderr_meta = req.value("capture_stderr_meta", false);
    int meta_stderr_idx = req.value("meta_stderr_idx", -1);
    if (capture_stderr_meta && meta_stderr_idx == -1) {
        meta_stderr_idx = static_cast<int>(chain.size() - 1);
    }

    DWORD out_buf_size = req.value("out_buffer_size", 1 << 20);
    if (out_buf_size == 0) out_buf_size = 1 << 20;

    auto pl = std::make_unique<Pipeline>();
    pl->id  = id;
    pl->job = create_kill_on_close_job();

    HANDLE nul_in  = open_nul(GENERIC_READ);
    HANDLE err_log = open_stderr_log();

    // Build the chain: each command's stdout feeds the next command's stdin.
    // The last command's stdout goes to a named pipe for the DLL to read.
    HANDLE prev_read = nul_in; // first command reads from NUL
    HANDLE meta_err_rd = nullptr;
    json resp = {{"ok", true}};

    for (size_t i = 0; i < chain.size(); ++i) {
        bool is_last = (i == chain.size() - 1);

        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE rd = nullptr, wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, out_buf_size)) {
            if (prev_read != nul_in) CloseHandle(prev_read);
            if (nul_in) CloseHandle(nul_in);
            if (err_log) CloseHandle(err_log);
            if (meta_err_rd) CloseHandle(meta_err_rd);
            return {{"ok", false}, {"error", "CreatePipe failed"}};
        }

        // The last stage's read-end is consumed by our proxy thread, so it MUST
        // NOT be inherited (otherwise the child holds a writer and EOF never
        // fires). Intermediate read-ends ARE inherited -- they're the next
        // child's stdin.
        if (is_last) SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

        HANDLE cmd_err = err_log;
        bool capture_this_meta = (static_cast<int>(i) == meta_stderr_idx);
        
        if (capture_this_meta) {
            HANDLE err_wr = nullptr;
            if (CreatePipe(&meta_err_rd, &err_wr, &sa, 1 << 16)) {
                SetHandleInformation(meta_err_rd, HANDLE_FLAG_INHERIT, 0);
                cmd_err = err_wr;
            }
        }

        std::wstring wcmd = widen(chain[i]);
        HANDLE proc       = spawn_in_job(pl->job, wcmd, prev_read, wr, cmd_err);
        CloseHandle(wr);
        if (capture_this_meta && cmd_err != err_log) {
            CloseHandle(cmd_err);
        }

        // Close the previous read-end now that the child inherited it (unless it
        // was nul_in, which we keep for the side command).
        if (prev_read != nul_in) CloseHandle(prev_read);

        if (!proc) {
            CloseHandle(rd);
            if (meta_err_rd) CloseHandle(meta_err_rd);
            if (nul_in) CloseHandle(nul_in);
            if (err_log) CloseHandle(err_log);
            return {{"ok", false}, {"error", "spawn failed for step " + std::to_string(i)}};
        }
        pl->processes.push_back(proc);

        if (is_last) {
            auto pipe_name = stream_pipe_name(g_token, id, L"pcm");
            HANDLE np      = create_stream_pipe(pipe_name, out_buf_size);
            if (np == INVALID_HANDLE_VALUE) {
                CloseHandle(rd);
                if (meta_err_rd) CloseHandle(meta_err_rd);
                if (nul_in) CloseHandle(nul_in);
                if (err_log) CloseHandle(err_log);
                return {{"ok", false}, {"error", "CreateNamedPipe failed for pcm"}};
            }
            
            resp["pcm_pipe"] = narrow(pipe_name);
            pl->proxies_data.push_back(std::make_unique<ProxyData>(rd, np, std::move(pipe_name)));
            pl->proxies.emplace_back(proxy_thread_fn, pl->proxies_data.back().get());
        
            if (meta_err_rd) {
                auto meta_name = stream_pipe_name(g_token, id, L"meta");
                HANDLE mnp     = create_stream_pipe(meta_name, 1 << 16);
                if (mnp != INVALID_HANDLE_VALUE) {
                    resp["meta_pipe"] = narrow(meta_name);
                    pl->proxies_data.push_back(
                        std::make_unique<ProxyData>(meta_err_rd, mnp, std::move(meta_name)));
                    pl->proxies.emplace_back(proxy_thread_fn, pl->proxies_data.back().get());
                } else {
                    CloseHandle(meta_err_rd);
                }
            }
        } else {
            prev_read = rd; // feed to next command's stdin
        }
    }

    // Optional side command (title resolver): its stdout goes to a separate
    // named pipe so the DLL can drain metadata independently.
    if (req.contains("side_cmd") && !req.at("side_cmd").get<std::string>().empty() && !meta_err_rd) {
        auto side_u8 = req.at("side_cmd").get<std::string>();
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE srd = nullptr, swr = nullptr;
        if (CreatePipe(&srd, &swr, &sa, 1 << 16)) {
            SetHandleInformation(srd, HANDLE_FLAG_INHERIT, 0);
            std::wstring wcmd = widen(side_u8);
            HANDLE sp         = spawn_in_job(pl->job, wcmd, nul_in, swr, err_log);
            CloseHandle(swr);
            if (sp) {
                pl->processes.push_back(sp);
                auto meta_name = stream_pipe_name(g_token, id, L"meta");
                HANDLE mnp     = create_stream_pipe(meta_name, 1 << 16);
                if (mnp != INVALID_HANDLE_VALUE) {
                    resp["meta_pipe"] = narrow(meta_name);
                    pl->proxies_data.push_back(
                        std::make_unique<ProxyData>(srd, mnp, std::move(meta_name)));
                    pl->proxies.emplace_back(proxy_thread_fn, pl->proxies_data.back().get());
                } else {
                    CloseHandle(srd);
                }
            } else {
                CloseHandle(srd);
            }
        }
    }

    if (nul_in) CloseHandle(nul_in);
    if (err_log) CloseHandle(err_log);

    std::scoped_lock lk{g_mu};
    g_pipelines[id] = std::move(pl);
    return resp;
}

// ---------------------------------------------------------------------------
// Handle "kill" -- terminate a pipeline.
// ---------------------------------------------------------------------------

json handle_kill(const json& req) {
    uint32_t id = req.at("id").get<uint32_t>();
    std::unique_ptr<Pipeline> pl;
    {
        std::scoped_lock lk{g_mu};
        if (auto it = g_pipelines.find(id); it != g_pipelines.end()) {
            pl = std::move(it->second);
            g_pipelines.erase(it);
        }
    }
    // pl's destructor (outside the lock) terminates children + joins threads.
    return {{"ok", true}};
}

// ---------------------------------------------------------------------------
// Serve one control connection: read a request, dispatch, reply, close.
// ---------------------------------------------------------------------------

void serve_connection(HANDLE pipe) {
    auto req_str = ipc_recv(pipe);
    if (!req_str.empty()) {
        json resp;
        try {
            auto msg = json::parse(req_str);
            auto op  = msg.at("op").get<std::string>();
            if (op == "shutdown") {
                ipc_send(pipe, json({{"ok", true}}).dump());
                FlushFileBuffers(pipe);
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
                shutdown_and_exit(0); // does not return
            } else if (op == "run") {
                resp = handle_run(msg);
            } else if (op == "spawn") {
                resp = handle_spawn(msg);
            } else if (op == "kill") {
                resp = handle_kill(msg);
            } else {
                resp = {{"ok", false}, {"error", "unknown op"}};
            }
        } catch (const std::exception& e) {
            resp = {{"ok", false}, {"error", e.what()}};
        }
        ipc_send(pipe, resp.dump(-1, ' ', false, json::error_handler_t::replace));
    }
    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
}

} // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    if (argc < 2) return 2; // session token is mandatory
    g_token    = widen(argv[1]);
    g_pipe_sa  = make_pipe_sa();

    // Self-terminate (taking our children with us) if the game/DLL dies without
    // a clean shutdown, so a crash never leaves yt-dlp/ffmpeg orphaned.
    if (argc >= 3) {
        DWORD parent_pid = std::strtoul(argv[2], nullptr, 10);
        if (HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parent_pid)) {
            std::thread([parent] {
                WaitForSingleObject(parent, INFINITE);
                CloseHandle(parent);
                shutdown_and_exit(0);
            }).detach();
        }
    }

    // Multi-instance accept loop: every request gets its own connection +
    // handler thread, so a slow capture can't stall an audio spawn/kill.
    const std::wstring ctrl_name = control_pipe_name(g_token);
    for (;;) {
        HANDLE h = CreateNamedPipeW(ctrl_name.c_str(), PIPE_ACCESS_DUPLEX,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                    PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, g_pipe_sa);
        if (h == INVALID_HANDLE_VALUE) return 1;

        BOOL connected = ConnectNamedPipe(h, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) {
            CloseHandle(h);
            continue;
        }
        std::thread(serve_connection, h).detach();
    }
}
