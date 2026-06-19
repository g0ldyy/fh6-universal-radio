#pragma once

// DLL-side client that delegates CreateProcess calls to an external worker
// process.  Eliminates the fork()-of-a-multi-GB-process penalty that causes
// in-game stutters under Wine/Proton.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace fh6::worker {

class WorkerClient {
public:
    WorkerClient() = default;
    ~WorkerClient();

    WorkerClient(const WorkerClient&) = delete;
    WorkerClient& operator=(const WorkerClient&) = delete;

    /// Launch the worker process and connect to its control pipe.
    bool start(const std::filesystem::path& worker_exe, 
                   const std::vector<std::pair<std::wstring, std::wstring>>& env_overrides = {});

    /// Send shutdown and wait for the worker to exit.
    void stop();

    /// True if the control pipe is connected and the worker process is alive.
    bool alive() const noexcept;

    // ---- synchronous operations ------------------------------------------

    /// Run a command, wait for it to finish, return captured output.
    /// @param capture_stderr  If true, capture stderr instead of stdout.
    /// @return The captured output, or empty string on failure.
    std::string run_capture(const std::wstring& cmd, bool capture_stderr = false);

    // ---- asynchronous pipeline operations --------------------------------

    struct SpawnResult {
        uint32_t pipeline_id = 0;
        HANDLE pcm_pipe  = nullptr;   // client-side handle to read PCM
        HANDLE meta_pipe = nullptr;   // client-side handle to read metadata (optional)
        bool ok = false;
    };

    /// Spawn a chain of piped commands (e.g. yt-dlp | ffmpeg) with an
    /// optional side command whose stdout is exposed as meta_pipe.
    SpawnResult spawn_pipeline(const std::vector<std::wstring>& chain,
                               const std::wstring& side_cmd = {},
                               bool capture_stderr_meta = false,
                               int meta_stderr_idx = -1,
                               uint32_t out_buffer_size = 0);

    /// Convenience: single command whose stdout is exposed as pcm_pipe.
    SpawnResult spawn_single(const std::wstring& cmd);

    /// Terminate all child processes of a pipeline.
    void kill_pipeline(uint32_t id);

private:
    /// Open a fresh control connection, send req, optionally read the reply, close.
    /// Each call is independent, so a slow capture never blocks a spawn/kill.
    /// want_response=false returns immediately after sending (fire-and-forget).
    std::string request(const std::string& req, bool want_response = true) const;

    std::wstring token_;            // per-session random pipe token (set by start)
    mutable std::mutex mu_;         // guards the process_ handle's lifecycle only
    HANDLE process_ = nullptr;
    std::atomic<uint32_t> next_id_{1};
};

} // namespace fh6::worker
