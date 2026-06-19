#pragma once
#include <atomic>
#include "fh6/audio_source.hpp"

namespace fh6::sources {

class VanillaRadioSource : public IAudioSource {
public:
    std::string_view name() const noexcept override { return "vanilla_radio"; }
    std::string_view display_name() const noexcept override { return "Vanilla Radio"; }
    bool initialize() override { return true; }
    void shutdown() noexcept override {}
    
    void play() override { state_.store(PlaybackState::playing, std::memory_order_relaxed); }
    void pause() override { state_.store(PlaybackState::paused, std::memory_order_relaxed); }
    void stop() override { state_.store(PlaybackState::stopped, std::memory_order_relaxed); }
    
    TrackInfo current_track() const override { return {"Vanilla Radio", "In-game Audio", "", ""}; }
    PlaybackState playback_state() const noexcept override {
        return state_.load(std::memory_order_relaxed);
    }
    AuthState auth_state() const noexcept override { return AuthState::none_required; }
    SourceCapabilities capabilities() const noexcept override { return {}; }
private:
    std::atomic<PlaybackState> state_{PlaybackState::stopped};
};

} // namespace fh6::sources