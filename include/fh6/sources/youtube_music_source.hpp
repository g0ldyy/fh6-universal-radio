#pragma once

#include "fh6/audio_source.hpp"
#include "fh6/config.hpp"

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
class YouTubeMusicSource final : public IAudioSource {
public:
    explicit YouTubeMusicSource(YouTubeMusicConfig cfg);
    ~YouTubeMusicSource() override;

    std::string_view name() const noexcept override { return "youtube_music"; }
    std::string_view display_name() const noexcept override { return "YouTube Music"; }

    bool initialize() override;
    void shutdown() noexcept override;

    void play() override;
    void pause() override;
    void stop() override;
    void next() override;
    void previous() override;
    void pump(RingBuffer& ring) override;

    // URL / playlist to play next.
    void set_target(std::string url);

    TrackInfo current_track() const override;
    PlaybackState playback_state() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }
    AuthState auth_state() const noexcept override { return auth_; }
    std::string auth_instructions() const override;
    SourceCapabilities capabilities() const noexcept override { return {false, true, true}; }

private:
    struct Pipe;

    void start_pipe_locked();    // mu_ held
    void stop_pipe_locked();     // mu_ held
    void resolve_queue_locked(); // mu_ held; populates queue_ from target_url_

    YouTubeMusicConfig cfg_;
    std::unique_ptr<Pipe> pipe_;

    mutable std::mutex mu_;
    std::string target_url_;
    std::vector<std::string> queue_; // canonical watch URLs in playback order
    std::size_t queue_idx_ = 0;
    std::string queue_built_for_; // value of target_url_ when queue_ was resolved
    TrackInfo info_{};
    std::atomic<uint64_t> position_ms_{0};
    int consecutive_failed_ = 0; // tracks-in-a-row that produced 0 PCM bytes
    AuthState auth_         = AuthState::none_required;
    std::atomic<PlaybackState> state_{PlaybackState::stopped};
};

} // namespace fh6::sources
