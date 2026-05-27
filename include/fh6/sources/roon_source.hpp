#pragma once

#include "fh6/audio_source.hpp"
#include "fh6/config.hpp"

#include <atomic>

namespace fh6::sources {

class RoonSource final : public IAudioSource {
public:
    explicit RoonSource(RoonConfig cfg);
    ~RoonSource() override;

    std::string_view name() const noexcept override { return "roon"; }
    std::string_view display_name() const noexcept override { return "Roon"; }

    bool initialize() override;
    void shutdown() noexcept override;

    void play() override;
    void pause() override;
    void stop() override;
    void next() override;
    void previous() override;
    void pump(RingBuffer& ring) override;

    TrackInfo current_track() const override;
    PlaybackState playback_state() const noexcept override;
    AuthState auth_state() const noexcept override;
    std::string auth_instructions() const override;
    SourceCapabilities capabilities() const noexcept override { return {false, true, false}; }

private:
    AuthState setup_state() const noexcept;

    RoonConfig cfg_;
    TrackInfo info_{};
    std::atomic<PlaybackState> state_{PlaybackState::stopped};
    std::atomic<bool> initialized_{false};
};

} // namespace fh6::sources
