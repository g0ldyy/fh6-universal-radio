#pragma once

#include "fh6/audio_source.hpp"
#include "fh6/config.hpp"
#include "fh6/playback_dsp.hpp"
#include "fh6/ring_buffer.hpp"
#include "fh6/worker/worker_client.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fh6::sources {

struct KoelTrack {
    std::string id;
    std::string parent;    // album ID (for cover art)
    std::string title;
    std::string artist;
    std::string album;
    std::string image_url; // Full URL or path to cover art
    std::uint64_t duration_ms = 0;
};

// Resolves a Koel playlist/album/favorites over Subsonic API HTTP, then streams
// each item through ffmpeg. The PCM pipe is drained by pump() in non-blocking
// mode (PeekNamedPipe-gated) so a stalled HTTP stream can never freeze the
// AudioSourceManager pump loop.
class KoelSource final : public IAudioSource {
public:
    KoelSource(KoelConfig cfg, std::filesystem::path ffmpeg_path,
               worker::WorkerClient* worker = nullptr);
    ~KoelSource() override;

    std::string_view name() const noexcept override { return "koel"; }
    std::string_view display_name() const noexcept override { return "Koel / Subsonic"; }

    bool initialize() override;
    void shutdown() noexcept override;

    void play() override;
    void pause() override;
    void stop() override;
    void next() override;
    void previous() override;
    void pump(RingBuffer& ring) override;

    // Settings drawer hot-update; re-fetches when auth/url/source fields
    // change, re-shuffles in place when only `shuffle` flips.
    void set_config(KoelConfig cfg);
    void set_ffmpeg_path(std::filesystem::path p);

    void set_playback_options(const PlaybackConfig& opts) override;

    // POST /api/source/koel/cast: swap to a different source type/id.
    // Returns empty string on success, error message on failure.
    std::string cast(std::string source_type, std::string source_id);

    std::string source_type() const;
    std::string auth_error() const;
    std::size_t track_count() const;

    TrackInfo current_track() const override;
    PlaybackState playback_state() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }
    AuthState auth_state() const noexcept override;
    SourceCapabilities capabilities() const noexcept override { return {false, true, true}; }

    std::string auth_instructions() const override {
        return "Enter your Koel or Subsonic-compatible server URL, your email or username, and your Subsonic API "
               "key as password. For Koel, the API key can be found in settings under 'Profile & Preferences' > 'Subsonic'.";
    }

private:
    struct Pipe;

    // mu_ held.
    std::unique_ptr<Pipe> spawn_pipe_locked(std::size_t for_idx);
    void start_pipe_locked();
    void stop_pipe_locked();
    void discard_prefetch_locked() noexcept;
    bool promote_prefetch_locked(std::size_t expected_idx);
    void maybe_spawn_prefetch_locked();
    std::size_t next_queue_idx_locked() const noexcept;
    void advance_locked(std::ptrdiff_t step);

    KoelConfig cfg_;
    std::filesystem::path ffmpeg_path_;
    worker::WorkerClient* worker_;

    mutable std::mutex mu_;
    std::vector<KoelTrack> queue_;
    std::size_t current_idx_ = 0;
    std::unique_ptr<Pipe> pipe_;
    std::unique_ptr<Pipe> prefetch_;
    std::atomic<PlaybackState> state_{PlaybackState::stopped};

    EqualizerStage eq_;
    std::atomic<bool> volume_norm_{false};
    std::atomic<bool> prebuffer_next_{true};
    std::atomic<AuthState> auth_state_{AuthState::needs_auth};
    std::string auth_error_;
    // Cache artwork URL per track so auth params don't change on every poll.
    mutable std::size_t cached_artwork_idx_ = SIZE_MAX;
    mutable std::string cached_artwork_url_;
};

} // namespace fh6::sources
