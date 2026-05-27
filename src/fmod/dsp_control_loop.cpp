#include "fh6/fmod/dsp_control_loop.hpp"
#include "fh6/fmod/dsp_retarget_policy.hpp"
#include "fh6/fmod/radio_discovery.hpp"
#include "fh6/fmod/radio_target_policy.hpp"
#include "fh6/audio_source_manager.hpp"
#include "fh6/log.hpp"

#include <chrono>
#include <cstring>

namespace fh6::fmod_bridge {

namespace {
using namespace std::chrono_literals;
constexpr auto kTick           = 20ms;
constexpr auto kDiscoveryRetry = 5s;
constexpr int kDiscoveryTries  = 120; // 10-minute budget; the radio system
                                      // isn't wired up until well into launch.

// Ticks of no read_callback progress (while the source is producing PCM)
// before we conclude the DSP is attached to a dead channel. 1s @ 20ms.
constexpr int kStaleTickThreshold = 50;

// SoundName of the placeholder sample our DSP overwrites. The documented
// setup enables FH6 Streamer Mode, which leaves a single station whose
// playlist can rotate through many SoundNames, so fallback is allowed there.
constexpr const char* kTargetSoundName = "HZ6_R9_PeterBroderick_EyesClosedandTraveling";
} // namespace

ControlLoop::ControlLoop(DSPBridge& bridge, const PEImage& img, float configured_gain)
    : bridge_{bridge}, img_{img}, configured_gain_{configured_gain},
      thread_{[this](const std::stop_token& tok) { run(tok); }} {}

ControlLoop::~ControlLoop() {
    thread_.request_stop();
    if (thread_.joinable()) thread_.join();
}

void ControlLoop::run(const std::stop_token& tok) {
    log::info("[ctrl] control loop started");

    DiscoveryResult disc;
    for (int attempt = 0; attempt < kDiscoveryTries && !tok.stop_requested(); ++attempt) {
        disc = discover_radio_instances(img_);
        if (select_instance(disc, /*require_live=*/false, nullptr)) break;
        for (auto t = std::chrono::steady_clock::now() + kDiscoveryRetry;
             std::chrono::steady_clock::now() < t && !tok.stop_requested();)
            std::this_thread::sleep_for(kTick);
    }

    const RadioInstance* chosen = select_instance(disc, /*require_live=*/false, nullptr);
    if (!chosen) {
        log::warn(R"([ctrl] target "{}" not found; media overlay or station is not active)",
                  kTargetSoundName);
        return;
    }

    void* fmod_system = resolve_fmod_system(img_, chosen->radio_stream);
    if (!fmod_system) {
        log::warn("[ctrl] FMOD SystemI resolution failed");
        return;
    }
    bridge_.set_target(*chosen, fmod_system);
    meta_.set_target(chosen->sample_props_body);
    log::info("[ctrl] targeting RadioStreamFmod @0x{:X} SoundName=\"{}\" SystemI*=0x{:X}",
              reinterpret_cast<uintptr_t>(chosen->radio_stream), chosen->sound_name,
              reinterpret_cast<uintptr_t>(fmod_system));

    // The radio HUD reads from the SampleProperties slots at a much lower
    // rate than the audio mixer. 4 Hz is more than enough and keeps the
    // memory writes off the hot path.
    constexpr int kMetaEveryNTicks = 12; // ~240 ms at the 20 ms tick rate.
    int meta_tick                  = 0;

    auto next = std::chrono::steady_clock::now();
    while (!tok.stop_requested()) {
        next += kTick;
        bridge_.retarget_if_needed();
        bridge_.manager().pump_once();

        if (++meta_tick >= kMetaEveryNTicks) {
            meta_tick = 0;
            push_metadata();
        }

        // Staleness watchdog: while a source is actively producing audio,
        // FMOD's mixer should be invoking our read_callback every tick.
        // If call_count() freezes for ~1s, the channel was destroyed by
        // FMOD without writing a fresh handle to +0x20 (e.g. placeholder
        // sample ended). Re-discover and switch to a live channel.
        auto* active          = bridge_.manager().active();
        const bool busy       = active && (active->playback_state() == PlaybackState::playing ||
                                     active->playback_state() == PlaybackState::buffering);
        const std::uint64_t c = bridge_.call_count();
        if (busy && c == prev_calls_) {
            if (++stale_ticks_ >= kStaleTickThreshold) {
                recover_stale_dsp();
                stale_ticks_ = 0;
            }
        } else {
            stale_ticks_ = 0;
        }
        prev_calls_ = c;

        const float target = [this, active] {
            if (!active) return 0.0f;
            switch (active->playback_state()) {
                case PlaybackState::playing:
                case PlaybackState::buffering:
                    return configured_gain_.load(std::memory_order_acquire);
                default: return 0.0f;
            }
        }();
        // 1-pole low-pass at ~100 ms so play/pause fades smoothly.
        const float cur = bridge_.gain();
        float next_g    = cur + (target - cur) * 0.1f;
        if (std::abs(next_g - cur) < 1e-4f) next_g = target;
        bridge_.set_gain(next_g);

        std::this_thread::sleep_until(next);
    }
    log::info("[ctrl] control loop exiting");
}

const RadioInstance* ControlLoop::select_instance(const DiscoveryResult& disc, bool require_live,
                                                  std::byte* avoid_radio_stream) const noexcept {
    return select_target_radio_instance(
        disc, kTargetSoundName, /*allow_fallback=*/true, require_live, avoid_radio_stream,
        [this](std::byte* radio_stream) { return bridge_.channel_handle_alive(radio_stream); });
}

void ControlLoop::recover_stale_dsp() noexcept {
    const bool current_alive        = bridge_.current_handle_alive();
    const uint32_t current_channels = bridge_.last_out_channels();
    if (is_stale_recovery_false_alarm(current_alive, current_channels)) return;

    auto disc                   = discover_radio_instances(img_);
    const RadioInstance* chosen = select_instance(
        disc, /*require_live=*/true, current_alive ? bridge_.target_radio_stream() : nullptr);
    if (!chosen) return;

    void* fmod_system = resolve_fmod_system(img_, chosen->radio_stream);
    if (!fmod_system) return;

    bridge_.set_target(*chosen, fmod_system);
    meta_.set_target(chosen->sample_props_body);
    log::info(R"([ctrl] DSP stale; recovered onto RadioStreamFmod @0x{:X} SoundName="{}")",
              reinterpret_cast<uintptr_t>(chosen->radio_stream), chosen->sound_name);
    // Next tick's retarget_if_needed installs the DSP on chosen's fresh handle.
}

void ControlLoop::push_metadata() noexcept {
    auto* a = bridge_.manager().active();
    if (!a) {
        meta_.update("FH6 Universal Radio", "Idle");
        return;
    }
    TrackInfo info;
    try {
        info = a->current_track();
    } catch (...) {
        return;
    }
    std::string title  = !info.title.empty() ? info.title : std::string{a->display_name()};
    std::string artist = info.artist;
    if (artist.empty()) {
        switch (a->playback_state()) {
            case PlaybackState::playing: artist = "Playing"; break;
            case PlaybackState::buffering: artist = "Buffering"; break;
            case PlaybackState::paused: artist = "Paused"; break;
            case PlaybackState::stopped: artist = "Stopped"; break;
        }
    }
    meta_.update(title, artist);
}

} // namespace fh6::fmod_bridge
