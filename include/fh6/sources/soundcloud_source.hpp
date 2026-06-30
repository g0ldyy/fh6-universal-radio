#pragma once

#include "fh6/audio_source.hpp"
#include "fh6/config.hpp"
#include "fh6/worker/worker_client.hpp"
#include "fh6/playback_dsp.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fh6::sources {

// Streams audio via `yt-dlp | ffmpeg -f s16le -ar 48000 -ac 2`. The PCM pipe
// is drained into the ring buffer by pump(). For playlist URLs we resolve the
// item list up front (via --flat-playlist) so next() / previous() can walk it.
class SoundCloudSource final : public IAudioSource {
public:
    SoundCloudSource(SoundCloudConfig cfg, std::filesystem::path ffmpeg_path,
                        worker::WorkerClient* worker = nullptr);
    ~SoundCloudSource() override;

    std::string_view name() const noexcept override { return "soundcloud"; }
    std::string_view display_name() const noexcept override { return "SoundCloud"; }
    uint64_t queue_version() const noexcept { return queue_version_.load(std::memory_order_acquire); }

    bool initialize() override;
    void shutdown() noexcept override;

    void play() override;
    void pause() override;
    void stop() override;
    void next() override;
    void previous() override;
    bool skip_next() override;
    bool restart_current() override;
    void pump(RingBuffer& ring) override;

    // URL / playlist to play next.
    void set_target(std::string url);

    void set_shuffle(bool shuffle);
    void set_ffmpeg_path(std::filesystem::path p);
    void set_yt_dlp_path(std::filesystem::path p);
    void set_playback_options(const PlaybackConfig& opts) override;

    void set_config(SoundCloudConfig cfg);
    void set_active_station(std::string name);

    std::size_t station_count() const noexcept;
    std::string active_station_name() const;

    struct QueueEntry {
        std::size_t index;
        std::string url;
        std::string title;
        std::string artist;
    };
    struct QueueSnapshot {
        std::size_t cursor;
        std::vector<QueueEntry> entries;
    };

    QueueSnapshot queue_snapshot() const;
    bool jump_to(std::size_t index);

    TrackInfo current_track() const override;
    PlaybackState playback_state() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }
    AuthState auth_state() const noexcept override { return auth_; }
    std::string auth_instructions() const override;
    SourceCapabilities capabilities() const noexcept override { return {false, true, true}; }

    bool shuffle() const {
        std::scoped_lock lk{mu_};
        return cfg_.shuffle;
    }

private:
    struct Pipe;

    // mu_ held for all *_locked helpers.
    std::unique_ptr<Pipe> spawn_pipe_locked(std::string_view url, std::size_t for_idx);
    void start_pipe_locked(); // (re)spawn pipe_ for queue_[queue_idx_]
    void stop_pipe_locked();  // drop pipe_ only
    void discard_prefetch_locked() noexcept;
    void resolve_queue_locked(); // populates queue_ from target_url_
    std::size_t next_queue_idx_locked() const noexcept;
    bool promote_prefetch_locked(std::size_t expected_idx);
    void maybe_spawn_prefetch_locked(); // called from pump() once current is healthy
    void drain_title_pipe_locked(Pipe* p);
    void hydrate_queue(uint64_t generation);

    SoundCloudConfig cfg_;
    std::filesystem::path ffmpeg_path_;
    std::filesystem::path yt_dlp_path_;
    worker::WorkerClient* worker_;
    std::unique_ptr<Pipe> pipe_;
    std::unique_ptr<Pipe> prefetch_; // pre-spawned next-track pipeline (or null)
    std::thread hydrate_thread_;
    std::vector<std::thread> old_hydrate_threads_;
    
    const SoundCloudStation* active_station_locked() const noexcept;

    mutable std::mutex mu_;
    std::string target_url_;
    struct InternalQueueEntry {
        std::string url;
        std::string title;
        std::string artist;
        std::size_t original_index = 0;
    };
    std::vector<InternalQueueEntry> queue_; // canonical URLs in playback order
    std::size_t queue_idx_ = 0;
    std::string queue_built_for_; // value of target_url_ when queue_ was resolved
    std::atomic<uint64_t> position_ms_{0};
    int consecutive_failed_ = 0; // tracks-in-a-row that produced 0 PCM bytes
    AuthState auth_         = AuthState::none_required;
    std::atomic<PlaybackState> state_{PlaybackState::stopped};

    EqualizerStage eq_;
    std::atomic<bool> volume_norm_{true};
    std::atomic<bool> prebuffer_next_{true};
    std::atomic<uint64_t> queue_generation_{0};
    std::atomic<uint64_t> queue_version_{0};
};

} // namespace fh6::sources