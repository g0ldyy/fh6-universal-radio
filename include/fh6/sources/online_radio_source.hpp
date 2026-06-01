#pragma once

#include "fh6/audio_source.hpp"
#include "fh6/config.hpp"
#include "fh6/playback_dsp.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fh6::sources {

class OnlineRadioSource final : public IAudioSource {
public:
    OnlineRadioSource(OnlineRadioConfig cfg, std::filesystem::path ffmpeg_path);
    ~OnlineRadioSource() override;

    std::string_view name() const noexcept override { return "online_radio"; }
    std::string_view display_name() const noexcept override { return "Online Radio"; }

    bool initialize() override;
    void shutdown() noexcept override;

    void play() override;
    void pause() override;
    void stop() override;
    void next() override;
    void previous() override;
    bool skip_next() override { next(); return true; }
    
    void pump(RingBuffer& ring) override;
    void set_playback_options(const PlaybackConfig& opts) override;

    TrackInfo current_track() const override;
    PlaybackState playback_state() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }
    AuthState auth_state() const noexcept override { return AuthState::none_required; }
    
    // capabilities: no seeking, but can go next/prev through the station list
    SourceCapabilities capabilities() const noexcept override { return {false, true, true}; }

    void set_config(const OnlineRadioConfig& c);
    void set_ffmpeg_path(std::filesystem::path p);
    void set_target(std::string url);

private:
    struct Pipe;
    void start_pipe_locked();
    void stop_pipe_locked();

    OnlineRadioConfig cfg_;
    std::filesystem::path ffmpeg_path_;
    std::unique_ptr<Pipe> pipe_;

    mutable std::mutex mu_;
    size_t current_station_idx_ = 0;
    std::string target_url_;
    
    // dynamic metadata state
    std::string current_title_;
    std::string current_artist_;
    
    std::atomic<PlaybackState> state_{PlaybackState::stopped};
    EqualizerStage eq_;
    std::atomic<bool> volume_norm_{true};
    std::atomic<uint64_t> position_ms_{0};
};

} // namespace fh6::sources