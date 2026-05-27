#pragma once

#include "fh6/audio_source.hpp"
#include "fh6/config.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace fh6::sources {

// Receives audio AirPlayed from a phone (Apple Music, etc.) and pumps it into
// the ring. We don't speak the AirPlay/RAOP protocol ourselves; instead we run
// an external receiver (shairport-sync `-o stdout`) and resample its raw PCM to
// the 48 kHz/stereo/s16le contract via ffmpeg, exactly like the YouTube source.
// The phone is the controller, so there is no queue, seek, or next/previous.
class AirPlaySource final : public IAudioSource {
public:
    explicit AirPlaySource(AirPlayConfig cfg);
    ~AirPlaySource() override;

    std::string_view name() const noexcept override { return "airplay"; }
    std::string_view display_name() const noexcept override { return "Apple Music (AirPlay)"; }

    bool initialize() override;
    void shutdown() noexcept override;

    void play() override;
    void pause() override;
    void stop() override;
    void pump(RingBuffer& ring) override;

    TrackInfo current_track() const override;
    PlaybackState playback_state() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }
    AuthState auth_state() const noexcept override { return AuthState::none_required; }
    std::string auth_instructions() const override;
    SourceCapabilities capabilities() const noexcept override { return {false, false, false}; }

    std::string service_name() const { return cfg_.service_name; }

private:
    struct Pipe;

    void start_pipe_locked();  // mu_ held
    void stop_pipe_locked();   // mu_ held

    AirPlayConfig cfg_;
    std::unique_ptr<Pipe> pipe_;

    mutable std::mutex mu_;
    std::atomic<uint64_t> position_ms_{0};
    std::atomic<PlaybackState> state_{PlaybackState::stopped};
};

} // namespace fh6::sources
