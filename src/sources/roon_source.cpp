#include "fh6/sources/roon_source.hpp"

#include <utility>

namespace fh6::sources {

RoonSource::RoonSource(RoonConfig cfg) : cfg_{std::move(cfg)} {}

RoonSource::~RoonSource() { shutdown(); }

bool RoonSource::initialize() {
    if (!cfg_.enabled) return false;
    initialized_.store(true, std::memory_order_release);
    state_.store(PlaybackState::stopped, std::memory_order_release);
    return true;
}

void RoonSource::shutdown() noexcept {
    state_.store(PlaybackState::stopped, std::memory_order_release);
    initialized_.store(false, std::memory_order_release);
}

void RoonSource::play() {
    if (initialized_.load(std::memory_order_acquire))
        state_.store(PlaybackState::playing, std::memory_order_release);
}

void RoonSource::pause() {
    if (initialized_.load(std::memory_order_acquire))
        state_.store(PlaybackState::paused, std::memory_order_release);
}

void RoonSource::stop() { state_.store(PlaybackState::stopped, std::memory_order_release); }

void RoonSource::next() { play(); }

void RoonSource::previous() { play(); }

void RoonSource::pump(RingBuffer&) {}

TrackInfo RoonSource::current_track() const { return info_; }

PlaybackState RoonSource::playback_state() const noexcept {
    return state_.load(std::memory_order_acquire);
}

AuthState RoonSource::auth_state() const noexcept { return setup_state(); }

std::string RoonSource::auth_instructions() const {
    if (cfg_.bridge_path.empty()) {
        return "Set [roon].bridge_path to tools\\roon-bridge\\index.mjs or another Roon sidecar "
               "script path.";
    }
    if (cfg_.selected_zone_id.empty() && cfg_.capture_device_id.empty()) {
        return "Authorize FH6 Universal Radio in Roon, then select a Roon zone and Windows "
               "capture device in Web Control.";
    }
    if (cfg_.selected_zone_id.empty()) {
        return "Select a Roon zone in Web Control.";
    }
    if (cfg_.capture_device_id.empty()) {
        return "Select a Windows capture device that receives the selected Roon zone audio.";
    }
    return {};
}

AuthState RoonSource::setup_state() const noexcept {
    if (cfg_.bridge_path.empty()) return AuthState::error;
    if (cfg_.selected_zone_id.empty() || cfg_.capture_device_id.empty())
        return AuthState::needs_auth;
    return AuthState::authenticated;
}

} // namespace fh6::sources
