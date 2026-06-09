#pragma once

#include "fh6/config.hpp"
#include "fh6/fmod/dsp_bridge.hpp"
#include "fh6/fmod/game_state_probe.hpp"
#include "fh6/fmod/metadata_injector.hpp"
#include "fh6/fmod/pe_image.hpp"
#include "fh6/fmod/radio_discovery.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

namespace fh6 {
class IAudioSource;
} // namespace fh6

namespace fh6::fmod_bridge {

class ControlLoop {
public:
    ControlLoop(DSPBridge& bridge, const PEImage& img, PlaybackConfig initial_playback,
                float configured_gain);
    ~ControlLoop();

    ControlLoop(const ControlLoop&)            = delete;
    ControlLoop& operator=(const ControlLoop&) = delete;

    void set_configured_gain(float g) noexcept {
        configured_gain_.store(g, std::memory_order_release);
    }

    void push_playback_options(PlaybackConfig opts);

private:
    using clock      = std::chrono::steady_clock;
    using time_point = clock::time_point;

    void run(const std::stop_token& tok);
    void push_metadata() noexcept;

    // Per-tick state-machine dispatch:
    // - quickStationSkip from the R10 edge (game_state_.on_target_station).
    // - raceStartPlayback from game_state_.race_active (FH6 radio_state
    //   +0x68 && +0x69) plus the race_restart helper at +0x80.
    void run_playback_state_machines(time_point now) noexcept;

    // Discover the live RadioStreamFmod carrying our sample and point the
    // bridge + metadata injector at it. Used at startup and after a recovery
    // toggle (the game may reallocate the wrapper).
    bool acquire_target() noexcept;
    const RadioInstance* select_instance(const DiscoveryResult& disc) const noexcept;

    DSPBridge& bridge_;
    const PEImage& img_;
    std::atomic<float> configured_gain_;
    std::atomic<int> mixer_pause_delay_ms_;
    MetadataInjector meta_;
    GameStateProbe game_state_;
    std::uint64_t prev_calls_ = 0;
    int stale_ticks_          = 0;
    int retune_attempts_      = 0;
    bool retune_suppressed_   = false;
    int mixer_idle_ticks_     = 0;
    bool mixer_consuming_     = true;
    int diagnostics_tick_      = 0;
    std::uint64_t prev_diagnostics_calls_ = 0;
    time_point last_retune_{};  // last off/on station toggle, for cooldown

    // std::atomic<std::shared_ptr<T>> would be ideal here but libc++ in
    // llvm-mingw doesn't ship the C++20 specialization; a plain mutex works
    // for both call sites (an occasional dashboard-driven store, an
    // every-tick load on the control loop) at negligible cost.
    mutable std::mutex playback_opts_mtx_;
    std::shared_ptr<const PlaybackConfig> playback_opts_;

    bool prev_r10_          = false;
    bool have_prev_r10_     = false;
    IAudioSource* active_source_ = nullptr;
    bool prev_radio_active_ = false;
    bool have_prev_radio_active_ = false;
    bool prev_dsp_active_ = false;
    bool have_prev_dsp_active_ = false;
    bool transport_active_ = false;
    bool have_transport_active_ = false;
    bool raw_transport_active_ = false;
    bool have_raw_transport_active_ = false;
    time_point last_raw_transport_flip_{};
    time_point prev_raw_transport_flip_{};
    time_point last_transport_change_{};
    time_point last_retarget_{};
    bool prev_race_         = false;
    bool prev_race_restart_ = false;
    bool quick_skip_armed_  = false;
    time_point last_r10_off_{};
    time_point last_race_event_{};
    time_point last_skip_cmd_{};

    std::jthread thread_;
};

} // namespace fh6::fmod_bridge
