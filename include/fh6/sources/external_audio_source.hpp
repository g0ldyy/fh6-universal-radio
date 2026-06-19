#pragma once

#include "fh6/audio_source.hpp"
#include "fh6/config.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <string>
#include <thread>
#include <vector>

namespace fh6::sources {

struct ExternalAudioDevice {
    std::string id;
    std::string name;
    bool is_default = false;
};

std::vector<ExternalAudioDevice> enumerate_external_audio_devices();

class ExternalAudioSource final : public IAudioSource {
public:
    ExternalAudioSource(ExternalAudioConfig cfg);
    ~ExternalAudioSource() override;

    std::string_view name() const noexcept override { return "external_audio"; }
    std::string_view display_name() const noexcept override { return "External Audio"; }

    bool initialize() override;
    void shutdown() noexcept override;

    void play() override;
    void pause() override;
    void stop() override;
    void next() override;
    void previous() override;
    bool skip_next() override;
    void pump(RingBuffer& ring) override;
    void on_radio_audible(bool audible) override;
    void set_config(ExternalAudioConfig cfg);

    TrackInfo current_track() const override;
    std::optional<ArtworkImage> artwork() const override;
    PlaybackState playback_state() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }
    AuthState auth_state() const noexcept override;
    std::string auth_instructions() const override;
    SourceCapabilities capabilities() const noexcept override {
        return SourceCapabilities{false, true, false};
    }

private:
    void start_worker();
    void stop_worker() noexcept;
    void capture_loop() noexcept;

    void append_pcm(const int16_t* data, std::size_t samples);
    void clear_queue();
    void compact_queue_locked();

    std::string configured_endpoint() const;
    std::string configured_media_session() const;
    void set_media_transport(bool play);

    mutable std::mutex meta_mu_;
    std::string device_name_ = "Default playback device";
    std::string last_error_;
    std::string endpoint_id_;
    std::string media_session_id_;
    // SMTC thumbnail cache, refreshed only when the track identity changes.
    mutable std::string art_key_;
    mutable ArtworkImage art_;

    TrackInfo cached_track_;

    std::mutex queue_mu_;
    std::vector<int16_t> pcm_queue_;
    std::size_t queue_offset_ = 0;

    std::mutex worker_mu_;
    std::thread worker_;
    std::atomic<bool> stop_requested_{false};

    std::atomic<PlaybackState> state_{PlaybackState::stopped};
    std::atomic<uint64_t> position_ms_{0};
    std::atomic<bool> media_active_{false};
};

} // namespace fh6::sources
