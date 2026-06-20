#include "fh6/fmod/dsp_control_loop.hpp"
#include "fh6/fmod/radio_discovery.hpp"
#include "fh6/audio_source.hpp"
#include "fh6/audio_source_manager.hpp"
#include "fh6/log.hpp"

#include <chrono>

namespace fh6::fmod_bridge {

namespace {
using namespace std::chrono_literals;
constexpr auto kTick           = 20ms;
constexpr auto kDiscoveryRetry = 5s;

// Ticks of no read_callback progress (while the source is producing PCM)
// before we conclude the game tore the radio channel down. 1s @ 20ms.
constexpr int kStaleTickThreshold = 50;

// Frozen-read ticks before we treat the station as genuinely silenced (pause
// menu, radio off) and tell the active source to mirror it onto any live
// player it wraps. Must clear the stall-retune rebuild window (~1s retune +
// up to ~2s reacquire), so a rewind or race transition that briefly tears the
// channel down and rebuilds it doesn't read as a pause. 3.5s @ 20ms.
constexpr int kInaudibleTicks = 175;

// Minimum gap between two off/on station toggles. The toggle blocks ~300ms
// and the game needs a moment to reallocate the channel, so we leave it well
// alone in between rather than thrashing the radio.
constexpr auto kRetuneCooldown = 6s;

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
    auto next = std::make_shared<const PlaybackConfig>(std::move(opts));
    std::lock_guard lock{playback_opts_mtx_};
    playback_opts_ = std::move(next);
}

ControlLoop::~ControlLoop() {
    thread_.request_stop();
    if (thread_.joinable()) thread_.join();
}

void ControlLoop::run(const std::stop_token& tok) {
    log::info("[ctrl] control loop started");

    while (!tok.stop_requested()) {
        if (acquire_target()) {
            break;
        }
        for (auto t = std::chrono::steady_clock::now() + kDiscoveryRetry;
             std::chrono::steady_clock::now() < t && !tok.stop_requested();)
            std::this_thread::sleep_for(kTick);
    }

    if (tok.stop_requested()) return;

    // The radio HUD reads from the SampleProperties slots at a much lower
    // rate than the audio mixer. 4 Hz is more than enough and keeps the
    // memory writes off the hot path.
    constexpr int kMetaEveryNTicks = 12; // ~240 ms at the 20 ms tick rate.
    int meta_tick                  = 0;

    auto next = std::chrono::steady_clock::now();
    while (!tok.stop_requested()) {
        const auto now = std::chrono::steady_clock::now();
        if (next < now) next = now;
        next += kTick;
        bridge_.retarget_if_needed();
        bridge_.manager().pump_once();

        auto* active_src = bridge_.manager().active();
        if (active_src && active_src->name() == "vanilla_radio") {
            bridge_.set_mode(DSPMode::passthrough);
        } else {
            bridge_.set_mode(DSPMode::pcm);
        }

        // Skip refreshing while the station is silenced (pause menu, rewind):
        // the HUD isn't shown, and freezing the value lets the dedup in
        // MetadataInjector swallow the resume so the game doesn't re-pop its
        // now-playing banner on every pause/rewind.
        if (++meta_tick >= kMetaEveryNTicks) {
            meta_tick = 0;
            if (radio_audible_) {
                // re-poll the discovery cache to grab the freshest SampleProperties body
                // the game often reallocates this object entirely when native stations change
                auto disc = discover_radio_instances(img_);
                if (const RadioInstance* current = select_instance(disc)) {
                    meta_.set_target(current->sample_props_body);
                }
                
                push_metadata();
            }
        }

        // Staleness watchdog: while a source is actively producing audio,
        // FMOD's mixer should be invoking our read_callback every tick. If
        // call_count() freezes for ~1s, the game tore down the radio channel
        // and won't rebuild it on its own. Toggling the in-game station off
        // and back on is the only thing that makes it allocate a fresh
        // channel; retarget_if_needed() then re-attaches the DSP next tick.
        // Gated on R10 so we never yank the user off a station they chose.
        auto* active          = bridge_.manager().active();
        const bool busy       = active && (active->playback_state() == PlaybackState::playing ||
                                     active->playback_state() == PlaybackState::buffering);
        const std::uint64_t c = bridge_.call_count();
        if (busy && c == prev_calls_) {
            if (++stale_ticks_ >= kStaleTickThreshold) {
                stale_ticks_ = 0;

                // verify the channel is actually dead, not just paused
                auto disc = discover_radio_instances(img_);
                const RadioInstance* current = select_instance(disc);
                const bool channel_dead = !current || !bridge_.channel_handle_alive(current->radio_stream);

                // Only retune when the channel is genuinely dead; if it's
                // still alive the game is just paused, so leave it be.
                if (channel_dead) {
                    const auto current_time = std::chrono::steady_clock::now();
                    if (current_time - last_retune_ >= kRetuneCooldown && game_state_.read().on_target_station &&
                        game_state_.retune_streamer_station()) {
                        last_retune_ = current_time;
                        // The toggle may hand us a freshly-allocated RadioStreamFmod;
                        // re-point at the live one so retarget_if_needed installs there.
                        acquire_target();
                    }
                }
            }
        } else {
            stale_ticks_ = 0;
        }

        // Audibility edge: while a source is producing, a frozen read_callback
        // means the game silenced our station. Report the transition so a
        // source wrapping a live player (External Audio) pauses/resumes it.
        // Re-arm on every source change and only count once that source has
        // actually been read, so stale loop state can't fire a phantom edge.
        // The threshold sits above the stall-retune rebuild window, so the
        // channel churn from a rewind or race transition (which the watchdog
        // above heals) never trips it; only sustained silence does.
        if (active && busy) {
            if (active != audible_source_) {
                audible_source_ = active;
                idle_ticks_     = 0;
                audible_primed_ = false;
                radio_audible_  = true;
            }
            if (c != prev_calls_) {
                idle_ticks_     = 0;
                audible_primed_ = true;
            } else if (audible_primed_) {
                ++idle_ticks_;
            }
            const bool audible = idle_ticks_ < kInaudibleTicks;
            if (audible != radio_audible_) {
                radio_audible_ = audible;
                active->on_radio_audible(audible);
            }
        } else {
            idle_ticks_     = 0;
            audible_primed_ = false;
            radio_audible_  = true;
            audible_source_ = nullptr;
        }

        run_playback_state_machines(now);
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

bool ControlLoop::acquire_target() noexcept {
    auto disc                   = discover_radio_instances(img_);
    const RadioInstance* chosen = select_instance(disc);
    if (!chosen) return false;
    if (chosen->sound_name != kTargetSoundName) {
        log::warn(R"([ctrl] no instance matches target "{}"; falling back to "{}")",
                  kTargetSoundName, chosen->sound_name);
    }

    void* fmod_system = resolve_fmod_system(img_, chosen->radio_stream);
    if (!fmod_system) {
        log::warn("[ctrl] FMOD SystemI resolution failed");
        return false;
    }
    bridge_.set_target(*chosen, fmod_system);
    meta_.set_target(chosen->sample_props_body);
    log::info("[ctrl] targeting RadioStreamFmod @0x{:X} SoundName=\"{}\" SystemI*=0x{:X}",
              reinterpret_cast<uintptr_t>(chosen->radio_stream), chosen->sound_name,
              reinterpret_cast<uintptr_t>(fmod_system));
    return true;
}

const RadioInstance* ControlLoop::select_instance(const DiscoveryResult& disc) const noexcept {
    const RadioInstance* target   = nullptr; // first placeholder-named match, any handle state
    const RadioInstance* fallback = nullptr; // first instance of any name
    for (const auto& i : disc.instances) {
        const bool is_target = i.sound_name == kTargetSoundName;
        // FH6 can spin up several streams sharing the placeholder name (e.g.
        // an idle secondary mix); prefer the one whose channel is actually
        // live so we attach to the stream that's carrying audio.
        if (is_target && bridge_.channel_handle_alive(i.radio_stream)) return &i;
        if (is_target && !target) target = &i;
        if (!fallback) fallback = &i;
    }
    return target ? target : fallback;
}

void ControlLoop::run_playback_state_machines(time_point now) noexcept {
    using namespace std::chrono_literals;
    // Debounce constants. 45 s ignores spurious race-flag flips during
    // loading screens; the 5 s race-restart window stays separate from the
    // 45 s race-start floor so a quick restart-then-engage still dispatches.
    constexpr auto kQuickSkipWindow     = 1000ms;
    constexpr auto kSkipCommandCooldown = 1500ms;
    constexpr auto kRaceStartDebounce   = 45s;
    constexpr auto kRaceRestartDebounce = 5s;

    std::shared_ptr<const PlaybackConfig> opts;
    {
        std::lock_guard lock{playback_opts_mtx_};
        opts = playback_opts_;
    }
    if (!opts) return;
    auto* active = bridge_.manager().active();
    if (!active) {
        prev_r10_ = prev_race_ = prev_race_restart_ = false;
        paused_by_race_off_ = false;
        first_connection_   = true;
        quick_skip_armed_   = false;
        return;
    }

    const auto game = game_state_.read();
    // R10 = "user is currently tuned to our station" via FH6 game state, NOT
    // FMOD channel aliveness. FMOD flaps the channel during race scene
    // transitions even though the user stayed on our station, which used to
    // trip a phantom quickStationSkip on every race start.
    const bool r10 = game.on_target_station;

    if (first_connection_) {
        prev_race_         = game.race_active;
        prev_race_restart_ = game.race_restart;
        prev_r10_          = r10;
        first_connection_  = false;
        return;
    }

    auto& ring     = bridge_.manager().ring();

    // --- raceStartPlayback (race_active edge, gated by R10 + debounces) ---
    const bool race_edge_in    = game.race_active && !prev_race_;
    const bool restart_edge_in = game.race_restart && !prev_race_restart_;
    const bool race_event      = (race_edge_in || restart_edge_in) && r10;
    const auto race_debounce   = restart_edge_in ? kRaceRestartDebounce : kRaceStartDebounce;
    if (race_event && now - last_race_event_ >= race_debounce &&
        now - last_skip_cmd_ >= kSkipCommandCooldown) {
        const auto& mode    = opts->race_start_playback;
        const char* outcome = "keeping current position";
        bool fired          = false;
        if (mode == "next") {
            fired   = active->skip_next();
            outcome = fired ? "advanced to next track" : "could not advance queue";
        } else if (mode == "restart") {
            fired   = active->restart_current();
            outcome = fired ? "restarted current track" : "could not restart current track";
        } else if (mode == "off") {
            const auto st = active->playback_state();
            if (st == PlaybackState::playing || st == PlaybackState::buffering) {
                active->stop();
                fired               = true;
                paused_by_race_off_ = true;
                outcome             = "stopped playback";
            } else {
                outcome = "skipped stop (not playing)";
            }
        }
        if (fired) {
            ring.drain();
            last_skip_cmd_ = now;
        }
        last_race_event_ = now;
        log::info("[ctrl] race {} -- {}", restart_edge_in ? "restarted" : "started", outcome);
    }

    // --- raceEndResume (race_active falling edge) ---
    const bool race_edge_out = !game.race_active && prev_race_;
    if (race_edge_out && paused_by_race_off_) {
        active->play();
        paused_by_race_off_ = false;
        log::info("[ctrl] race ended -- resuming playback");
    }
    prev_race_         = game.race_active;
    prev_race_restart_ = game.race_restart;

    // --- quickStationSkip (R10 edge) ---
    if (prev_r10_ && !r10) {
        last_r10_off_ = now;
        if (opts->quick_station_skip) quick_skip_armed_ = true;
    } else if (!prev_r10_ && r10) {
        if (quick_skip_armed_ && now - last_r10_off_ <= kQuickSkipWindow &&
            now - last_skip_cmd_ >= kSkipCommandCooldown) {
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

    // if using vanilla radio, let the game handle its own native metadata
    // reset the injector cache so that when we swap to a custom source,
    // it will overwrite whatever native metadata the game wrote in the meantime
    if (a->name() == "vanilla_radio") {
        meta_.reset_cache();
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
