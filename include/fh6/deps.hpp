#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fh6 {

// Order doubles as the slot index.
enum class Tool { ffmpeg, yt_dlp, librespot, texconv };
constexpr std::size_t kToolCount = 4;

struct DependencyStatus {
    std::string name;
    bool managed_present           = false;
    bool downloading               = false;
    std::uint64_t downloaded_bytes = 0;
    std::uint64_t total_bytes      = 0;
    std::string error;
};

class DependencyManager {
public:
    explicit DependencyManager(std::filesystem::path bin_dir);
    ~DependencyManager();

    DependencyManager(const DependencyManager&)            = delete;
    DependencyManager& operator=(const DependencyManager&) = delete;

    // on_tool_ready fires on the worker thread after each binary lands, so a
    // source can pick up the new path without a restart.
    void start(std::function<void()> on_tool_ready);
    void retry();

    // User override > managed bin/ copy > empty (caller's PATH fallback).
    std::filesystem::path resolve(Tool tool, const std::filesystem::path& user_override) const;

    std::vector<DependencyStatus> snapshot() const;

private:
    struct Slot {
        std::atomic<std::uint64_t> downloaded{0};
        std::atomic<std::uint64_t> total{0};
        std::atomic<bool> downloading{false};
        std::atomic<bool> present{false};
        mutable std::mutex err_mu;
        std::string error;
    };

    void run_worker();

    std::filesystem::path bin_dir_;
    std::array<Slot, kToolCount> slots_;
    std::function<void()> on_ready_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
};

} // namespace fh6
