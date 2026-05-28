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
#include <stop_token>
#include <thread>

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
    // - quickStationSkip from DSPBridge::current_handle_alive() (jitter-free
    //   FMOD channel slot read at radio_stream+0x20).
    // - raceStartPlayback from game_state_.race_active (FH6 radio_state
    //   +0x68 && +0x69) plus the race_restart helper at +0x80.
    void run_playback_state_machines(time_point now) noexcept;

    // Pick the best RadioInstance from a discovery result, preferring the
    // target SoundName and optionally filtering to those whose +0x20 holds
    // a live FMOD channel handle (used by recovery).
    const RadioInstance* select_instance(const DiscoveryResult& disc, bool require_live,
                                         std::byte* avoid_radio_stream) const noexcept;

    // Re-discover and switch to an instance with a live channel handle when
    // our DSP has stopped receiving reads (channel destroyed by FMOD with
    // no replacement written to +0x20).
    void recover_stale_dsp() noexcept;

    DSPBridge& bridge_;
    const PEImage& img_;
    std::atomic<float> configured_gain_;
    MetadataInjector meta_;
    GameStateProbe game_state_;
    std::uint64_t prev_calls_ = 0;
    int stale_ticks_          = 0;

    std::atomic<std::shared_ptr<const PlaybackConfig>> playback_opts_;

    bool prev_r10_          = false;
    bool prev_race_         = false;
    bool prev_race_restart_ = false;
    bool quick_skip_armed_  = false;
    time_point last_r10_off_{};
    time_point last_race_event_{};
    time_point last_skip_cmd_{};

    std::jthread thread_;
};

} // namespace fh6::fmod_bridge
