#include "fh6/fmod/dsp_control_loop.hpp"
#include "fh6/fmod/radio_discovery.hpp"
#include "fh6/audio_source.hpp"
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

// SoundName of the placeholder sample our DSP overwrites. Matches the carrier
// shipped by the radio-mod media overlay; if absent, we fall back to the
// first chain-valid instance so a stale overlay doesn't silently break audio.
constexpr const char* kTargetSoundName = "HZ6_R9_PeterBroderick_EyesClosedandTraveling";
} // namespace

ControlLoop::ControlLoop(DSPBridge& bridge, const PEImage& img, PlaybackConfig initial_playback,
                         float configured_gain)
    : bridge_{bridge}, img_{img}, configured_gain_{configured_gain}, game_state_{img},
      playback_opts_{std::make_shared<const PlaybackConfig>(std::move(initial_playback))},
      thread_{[this](const std::stop_token& tok) { run(tok); }} {}

void ControlLoop::push_playback_options(PlaybackConfig opts) {
    playback_opts_.store(std::make_shared<const PlaybackConfig>(std::move(opts)),
                         std::memory_order_release);
}

ControlLoop::~ControlLoop() {
    thread_.request_stop();
    if (thread_.joinable()) thread_.join();
}

void ControlLoop::run(const std::stop_token& tok) {
    log::info("[ctrl] control loop started");

    DiscoveryResult disc;
    for (int attempt = 0; attempt < kDiscoveryTries && !tok.stop_requested(); ++attempt) {
        disc = discover_radio_instances(img_);
        if (!disc.instances.empty()) break;
        for (auto t = std::chrono::steady_clock::now() + kDiscoveryRetry;
             std::chrono::steady_clock::now() < t && !tok.stop_requested();)
            std::this_thread::sleep_for(kTick);
    }

    if (disc.instances.empty()) {
        log::warn("[ctrl] discovery timed out; control loop exiting");
        return;
    }

    const RadioInstance* chosen = select_instance(disc, /*require_live=*/false);
    if (!chosen) {
        log::warn("[ctrl] discovery returned no usable instance; control loop exiting");
        return;
    }
    if (chosen->sound_name != kTargetSoundName) {
        log::warn(R"([ctrl] no instance matches target "{}"; falling back to "{}")",
                  kTargetSoundName, chosen->sound_name);
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

        run_playback_state_machines(std::chrono::steady_clock::now());
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

const RadioInstance* ControlLoop::select_instance(const DiscoveryResult& disc,
                                                  bool require_live) const noexcept {
    const RadioInstance* fallback = nullptr;
    for (auto& i : disc.instances) {
        if (require_live && !bridge_.channel_handle_alive(i.radio_stream)) continue;
        if (i.sound_name == kTargetSoundName) return &i;
        if (!fallback) fallback = &i;
    }
    return fallback;
}

void ControlLoop::recover_stale_dsp() noexcept {
    if (bridge_.current_handle_alive()) return;  // false alarm; channel still live

    auto disc = discover_radio_instances(img_);
    const RadioInstance* chosen = select_instance(disc, /*require_live=*/true);
    if (!chosen) return;

    void* fmod_system = resolve_fmod_system(img_, chosen->radio_stream);
    if (!fmod_system) return;

    bridge_.set_target(*chosen, fmod_system);
    meta_.set_target(chosen->sample_props_body);
    log::info(R"([ctrl] DSP stale; recovered onto RadioStreamFmod @0x{:X} SoundName="{}")",
              reinterpret_cast<uintptr_t>(chosen->radio_stream), chosen->sound_name);
    // Next tick's retarget_if_needed installs the DSP on chosen's fresh handle.
}

void ControlLoop::run_playback_state_machines(time_point now) noexcept {
    using namespace std::chrono_literals;
    // Debounce constants. 45 s ignores spurious race-flag flips during
    // loading screens; the 5 s race-restart window stays separate from the
    // 45 s race-start floor so a quick restart-then-engage still dispatches.
    constexpr auto kQuickSkipWindow     = 1000ms;
    constexpr auto kSpircCooldown       = 1500ms;
    constexpr auto kRaceStartDebounce   = 45s;
    constexpr auto kRaceRestartDebounce = 5s;

    auto opts = playback_opts_.load(std::memory_order_acquire);
    if (!opts) return;
    auto* active = bridge_.manager().active();
    if (!active) {
        prev_r10_ = prev_race_ = prev_race_restart_ = false;
        quick_skip_armed_ = false;
        return;
    }

    const auto game = game_state_.read();
    // R10 = "user is currently tuned to our station" via FH6 game state, NOT
    // FMOD channel aliveness. FMOD flaps the channel during race scene
    // transitions even though the user stayed on our station, which used to
    // trip a phantom quickStationSkip on every race start.
    const bool r10 = game.on_target_station;
    auto& ring     = bridge_.manager().ring();

    // --- raceStartPlayback (race_active edge, gated by R10 + debounces) ---
    const bool race_edge_in    = game.race_active && !prev_race_;
    const bool restart_edge_in = game.race_restart && !prev_race_restart_;
    const bool race_event      = (race_edge_in || restart_edge_in) && r10;
    const auto race_debounce   = restart_edge_in ? kRaceRestartDebounce : kRaceStartDebounce;
    if (race_event && now - last_race_event_ >= race_debounce &&
        now - last_skip_cmd_ >= kSpircCooldown) {
        const auto& mode    = opts->race_start_playback;
        const char* outcome = "keeping current position";
        bool fired          = false;
        if (mode == "next") {
            fired   = active->skip_next();
            outcome = fired ? "advanced to next track" : "could not advance queue";
        } else if (mode == "restart") {
            fired   = active->restart_current();
            outcome = fired ? "restarted current track" : "could not restart current track";
        }
        if (fired) {
            ring.drain();
            last_skip_cmd_ = now;
        }
        last_race_event_ = now;
        log::info("[ctrl] race {} -- {}", restart_edge_in ? "restarted" : "started", outcome);
    }
    prev_race_         = game.race_active;
    prev_race_restart_ = game.race_restart;

    // --- quickStationSkip (R10 edge) ---
    if (prev_r10_ && !r10) {
        last_r10_off_ = now;
        if (opts->quick_station_skip) quick_skip_armed_ = true;
    } else if (!prev_r10_ && r10) {
        if (quick_skip_armed_ && now - last_r10_off_ <= kQuickSkipWindow &&
            now - last_skip_cmd_ >= kSpircCooldown) {
            if (active->skip_next()) {
                ring.drain();
                last_skip_cmd_ = now;
                log::info("[ctrl] quick station return -- advanced to next track");
            }
        }
        quick_skip_armed_ = false;
    }
    prev_r10_ = r10;
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
            case PlaybackState::playing:   artist = "Playing"; break;
            case PlaybackState::buffering: artist = "Buffering"; break;
            case PlaybackState::paused:    artist = "Paused"; break;
            case PlaybackState::stopped:   artist = "Stopped"; break;
        }
    }
    meta_.update(title, artist);
}

} // namespace fh6::fmod_bridge
