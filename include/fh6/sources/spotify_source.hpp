#pragma once

#include "fh6/audio_source.hpp"
#include "fh6/config.hpp"
#include "fh6/worker/worker_client.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace fh6::sources {

class SpotifySource final : public IAudioSource {
public:
    SpotifySource(SpotifyConfig cfg, std::filesystem::path ffmpeg_path, worker::WorkerClient* worker = nullptr);
    ~SpotifySource() override;

    std::string_view name() const noexcept override { return "spotify"; }
    std::string_view display_name() const noexcept override { return "Spotify Connect"; }

    bool initialize() override;
    void shutdown() noexcept override;

    // Settings drawer hot-update; new paths apply on the next pipe start.
    void set_config(SpotifyConfig cfg, std::filesystem::path ffmpeg_path);
    void set_playback_options(const PlaybackConfig& opts) override;

    void play() override;
    void pause() override;
    void stop() override;
    void next() override; // next/prev are handled by OS media keys
    void previous() override;
    bool restart_current() override;
    void pump(RingBuffer& ring) override;

    TrackInfo current_track() const override;
    PlaybackState playback_state() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }
    AuthState auth_state() const noexcept override;
    std::string auth_instructions() const override;
    SourceCapabilities capabilities() const noexcept override {
        return {/*seek*/ false, /*previous*/ true, /*queue*/ false};
    }

private:
    struct Pipe;

    void start_pipe_locked();          // mu_ held
    void stop_pipe_locked();           // mu_ held
    void transport_skip(bool forward); // shared next()/previous() body
    bool cache_exists() const;
    bool volume_normalization_ = false;

    SpotifyConfig cfg_;
    std::filesystem::path ffmpeg_path_;
    worker::WorkerClient* worker_;
    std::unique_ptr<Pipe> pipe_;

    mutable std::mutex mu_;
    TrackInfo info_{};
    std::atomic<PlaybackState> state_{PlaybackState::stopped};
};

} // namespace fh6::sources
