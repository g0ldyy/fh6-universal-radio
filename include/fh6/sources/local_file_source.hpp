#pragma once

#include "fh6/audio_source.hpp"
#include "fh6/config.hpp"
#include "fh6/playback_dsp.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

namespace fh6::sources {

class LocalFileSource final : public IAudioSource {
public:
    LocalFileSource(LocalFilesConfig cfg, std::filesystem::path ffmpeg_path);
    ~LocalFileSource() override;

    std::string_view name() const noexcept override { return "local_files"; }
    std::string_view display_name() const noexcept override { return "Local Files"; }

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

    void set_directory(std::filesystem::path dir, bool recursive);
    void set_shuffle(bool shuffle);
    void set_ffmpeg_path(std::filesystem::path p);
    void set_playback_options(const PlaybackConfig& opts) override;
    std::vector<std::string> playlist_snapshot() const;

    TrackInfo current_track() const override;
    PlaybackState playback_state() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }

    AuthState auth_state() const noexcept override;
    std::string auth_instructions() const override;
    std::size_t track_count() const noexcept;

    SourceCapabilities capabilities() const noexcept override { return {true, true, false}; }

private:
    void rebuild_playlist();
    bool open_track(std::size_t index);
    bool open_track_ffmpeg(const std::filesystem::path& path);
    void close_current();

    LocalFilesConfig cfg_;
    std::filesystem::path ffmpeg_path_;
    std::vector<std::filesystem::path> playlist_;
    std::size_t cursor_ = 0;

    struct Decoder; // pimpl, keeps miniaudio out of the header
    std::unique_ptr<Decoder> dec_;

    mutable std::mutex mu_;
    std::atomic<PlaybackState> state_{PlaybackState::stopped};
    std::atomic<uint64_t> position_ms_{0};

    EqualizerStage eq_;
    std::atomic<bool> volume_norm_{true};
    std::atomic<float> loudness_coef_{1.0f}; // per-track multiplier, computed in open_track()
};

} // namespace fh6::sources
