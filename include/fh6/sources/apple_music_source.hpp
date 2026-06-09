#pragma once

#include "fh6/audio_source.hpp"
#include "fh6/config.hpp"
#include "fh6/playback_dsp.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

struct IAudioClient;
struct IAudioCaptureClient;
struct IMMDevice;
struct IMMDeviceEnumerator;
typedef struct tWAVEFORMATEX WAVEFORMATEX;

namespace fh6::sources {

// Captures authorized Apple Music playback from the default Windows output
// device using WASAPI loopback. This is intentionally not URL based: full
// Apple Music tracks are DRM protected, so the supported path is "play it in
// Apple Music, then feed the resulting system audio into FH6".
class AppleMusicSource final : public IAudioSource {
public:
    explicit AppleMusicSource(AppleMusicConfig cfg);
    ~AppleMusicSource() override;

    std::string_view name() const noexcept override { return "apple_music"; }
    std::string_view display_name() const noexcept override { return "Apple Music"; }

    bool initialize() override;
    void shutdown() noexcept override;

    void play() override;
    void pause() override;
    void stop() override;
    void next() override;
    void previous() override;
    void pump(RingBuffer& ring) override;

    void set_config(AppleMusicConfig cfg);
    void set_playback_options(const PlaybackConfig& opts) override;
    void on_radio_active_changed(bool active) override;
    void on_audio_sink_active_changed(bool active) override;
    void on_game_foreground_changed(bool foreground) override;
    bool consume_drain_request() noexcept override;

    TrackInfo current_track() const override;
    std::optional<ArtworkImage> artwork() const override;
    PlaybackState playback_state() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }
    AuthState auth_state() const noexcept override { return auth_; }
    std::string auth_instructions() const override;
    SourceCapabilities capabilities() const noexcept override { return {false, true, false}; }

private:
    void close_capture_locked() noexcept;
    bool ensure_capture_locked();
    bool ensure_process_loopback_locked();
    bool ensure_device_capture_locked();
    void send_media_key(uint16_t vk) const;
    enum class MediaCommand { play, pause, next, previous };
    bool refresh_media_session_locked(bool force = false);
    bool send_session_command_locked(MediaCommand cmd);
    void set_external_mute_locked(bool muted) noexcept;
    void restore_external_mute_locked() noexcept;
    bool device_capture_requested_locked() const noexcept;
    bool ensure_monitor_locked() noexcept;
    void close_monitor_locked() noexcept;
    void reclaim_monitor_blocks_locked() noexcept;
    void render_monitor_locked(const int16_t* samples, std::size_t frames) noexcept;
    void append_frames(const void* data, uint32_t frames, uint32_t flags, RingBuffer& ring);
    void refresh_track_locked(bool force = false);
    void refresh_artwork_locked(const std::string& key);
    void flush_capture_packets_locked() noexcept;

    AppleMusicConfig cfg_;

    mutable std::mutex mu_;
    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* device_ = nullptr;
    IAudioClient* audio_client_ = nullptr;
    IAudioCaptureClient* capture_client_ = nullptr;
    WAVEFORMATEX* mix_format_ = nullptr;
    bool com_initialized_ = false;
    bool capture_started_ = false;
    bool using_device_capture_ = false;
    bool radio_active_ = false;
    bool audio_sink_active_ = false;
    bool game_foreground_ = true;
    bool show_album_in_hud_ = true;
    void* monitor_wave_out_ = nullptr;
    struct MonitorBlock;
    std::vector<std::unique_ptr<MonitorBlock>> monitor_blocks_;

    AuthState auth_ = AuthState::none_required;
    std::atomic<PlaybackState> state_{PlaybackState::stopped};
    std::atomic<uint64_t> position_ms_{0};
    bool paused_by_radio_ = false;
    bool drain_requested_ = false;
    std::string last_track_key_;
    uint32_t mute_refresh_ticks_ = 0;
    uint32_t metadata_refresh_ticks_ = 0;
    std::chrono::steady_clock::time_point ignore_silent_capture_until_{};
    TrackInfo cached_track_;
    ArtworkImage cached_art_;
    std::string cached_art_key_;

    struct MutedSession;
    std::vector<MutedSession> muted_sessions_;

    EqualizerStage eq_;
    std::vector<int16_t> scratch_;
    uint64_t resample_accum_ = 0;
    struct MediaSessionState;
    std::unique_ptr<MediaSessionState> media_;
};

} // namespace fh6::sources
