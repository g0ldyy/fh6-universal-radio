#pragma once

#include "fh6/fmod/dsp_bridge.hpp"
#include "fh6/fmod/metadata_injector.hpp"
#include "fh6/fmod/pe_image.hpp"
#include "fh6/fmod/radio_discovery.hpp"

#include <atomic>
#include <cstdint>
#include <stop_token>
#include <thread>

namespace fh6::fmod_bridge {

// 50 Hz tick: keeps the DSP installed on the current radio channel and
// drives the AudioSourceManager pump.
class ControlLoop {
public:
    ControlLoop(DSPBridge& bridge, const PEImage& img, float configured_gain);
    ~ControlLoop();

    ControlLoop(const ControlLoop&)            = delete;
    ControlLoop& operator=(const ControlLoop&) = delete;

    void set_configured_gain(float g) noexcept {
        configured_gain_.store(g, std::memory_order_release);
    }

private:
    void run(const std::stop_token& tok);
    void push_metadata() noexcept;

    // Pick the best RadioInstance from a discovery result, preferring the
    // target SoundName and optionally filtering to those whose +0x20 holds
    // a live FMOD channel handle (used by recovery).
    const RadioInstance* select_instance(const DiscoveryResult& disc,
                                         bool require_live) const noexcept;

    // Re-discover and switch to an instance with a live channel handle when
    // our DSP has stopped receiving reads (channel destroyed by FMOD with
    // no replacement written to +0x20).
    void recover_stale_dsp() noexcept;

    void poll_media_keys() noexcept;

    DSPBridge& bridge_;
    const PEImage& img_;
    std::atomic<float> configured_gain_;
    MetadataInjector meta_;
    std::uint64_t prev_calls_ = 0;
    int stale_ticks_          = 0;
    bool prev_next_key_       = false; // edge-detect VK_MEDIA_NEXT_TRACK
    bool prev_prev_key_       = false; // edge-detect VK_MEDIA_PREV_TRACK
    std::jthread thread_;
};

} // namespace fh6::fmod_bridge
