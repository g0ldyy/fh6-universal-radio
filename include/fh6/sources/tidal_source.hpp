#pragma once

#include "fh6/audio_source.hpp"
#include "fh6/config.hpp"
#include "fh6/config_store.hpp"
#include "fh6/playback_dsp.hpp"
#include "fh6/ring_buffer.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fh6::sources {

struct TidalTrack {
    std::string id;
    std::string title;
    std::string artist;
    std::string album;
    std::string artwork_url;
    uint64_t duration_ms = 0;
};

// Streams TIDAL audio tracks by fetching dynamic stream/manifest URLs via the
// TIDAL API, then passing them into ffmpeg to pipe PCM.
// Handles OAuth2 Device Authorization Flow automatically using WinHTTP.
class TidalSource final : public IAudioSource {
public:
    TidalSource(TidalConfig cfg, std::filesystem::path ffmpeg_path, ConfigStore& store);
    ~TidalSource() override;

    std::string_view name() const noexcept override         { return "tidal"; }
    std::string_view display_name() const noexcept override { return "Tidal Music"; }

    bool initialize() override;
    void shutdown() noexcept override;

    void play() override;
    void pause() override;
    void stop() override;
    void next() override;
    void previous() override;
    void pump(RingBuffer& ring) override;

    // Hot configuration updates from the dashboard settings drawer.
    void set_config(TidalConfig cfg);
    void set_ffmpeg_path(std::filesystem::path p);

    // Dynamic playlist cast (e.g. POST /api/source/tidal/cast)
    bool cast(std::string playlist_id);

    // Exchange standard OAuth2 Authorization Code
    bool exchange_authorization_code(const std::string& code);

    void set_playback_options(const PlaybackConfig& opts) override;

    TrackInfo current_track() const override;

    struct QueueSnapshot {
        std::size_t cursor = 0;
        std::vector<TidalTrack> entries;
    };
    QueueSnapshot queue_snapshot() const;
    bool jump_to(std::size_t index);
    std::size_t track_count() const;
    std::size_t current_index() const;
    PlaybackState playback_state() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }
    AuthState auth_state() const noexcept override {
        return auth_.load(std::memory_order_acquire);
    }
    std::string auth_instructions() const override;
    SourceCapabilities capabilities() const noexcept override { return {false, true, true}; }

private:
    struct Pipe;

    // mu_ held
    bool refresh_queue_locked();
    std::unique_ptr<Pipe> spawn_pipe_locked(std::size_t for_idx);
    void start_pipe_locked();
    void stop_pipe_locked();
    void discard_prefetch_locked() noexcept;
    bool promote_prefetch_locked(std::size_t expected_idx);
    void maybe_spawn_prefetch_locked();
    std::size_t next_queue_idx_locked() const noexcept;
    void advance_locked(std::ptrdiff_t step);

    // Background thread for Device Code polling & Token refreshing
    void run_auth_loop();
    void stop_auth_thread() noexcept;
    bool refresh_access_token();

    TidalConfig cfg_;
    std::filesystem::path ffmpeg_path_;
    ConfigStore& store_;

    mutable std::mutex mu_;
    std::vector<TidalTrack> queue_;
    std::size_t current_idx_ = 0;
    std::unique_ptr<Pipe> pipe_;
    std::unique_ptr<Pipe> prefetch_;
    std::atomic<PlaybackState> state_{PlaybackState::stopped};

    // OAuth2 Device Auth state
    std::atomic<AuthState> auth_{AuthState::none_required};
    std::string auth_user_code_;
    std::string auth_verification_uri_;
    std::thread auth_thread_;
    std::atomic<bool> stop_auth_thread_{false};

    EqualizerStage eq_;
    std::atomic<bool> volume_norm_{false};
    std::atomic<bool> prebuffer_next_{true};
};

} // namespace fh6::sources
