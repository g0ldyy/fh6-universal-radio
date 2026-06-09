#include "fh6/fmod/dsp_control_loop.hpp"
#include "fh6/fmod/radio_discovery.hpp"
#include "fh6/audio_source.hpp"
#include "fh6/audio_source_manager.hpp"
#include "fh6/log.hpp"

#include <algorithm>
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
// before we conclude the game tore the radio channel down. 1s @ 20ms.
constexpr int kStaleTickThreshold = 50;
constexpr int kMaxRetuneAttemptsPerStall = 3;

// Minimum gap between two off/on station toggles. The toggle blocks ~300ms
// and the game needs a moment to reallocate the channel, so we leave it well
// alone in between rather than thrashing the radio.
constexpr auto kRetuneCooldown = 6s;

// Apple Music transport follows the old "station + mixer" pause detection,
// but with hysteresis so FH6 channel reallocations don't hammer media keys.
// A lone clean flip applies quickly. Repeated flips enter a quiet-period latch;
// only the final remembered state applies after the signal calms down.
constexpr auto kTransportJitterWindow = 300ms;
constexpr auto kTransportQuietPeriod = 300ms;
constexpr auto kTransportMinToggleGap = 0ms;
constexpr auto kRetargetTransportGrace = 5s;

// SoundName of the placeholder sample our DSP prefers to overwrite. The
// media overlay can cycle through other R9 tracks, so any R9 RadioStreamFmod
// is a valid custom-radio carrier. Built-in stations are not.
constexpr const char* kTargetSoundName = "HZ6_R9_PeterBroderick_EyesClosedandTraveling";
constexpr const char* kCustomStationPrefix = "HZ6_R9_";

} // namespace

ControlLoop::ControlLoop(DSPBridge& bridge, const PEImage& img, PlaybackConfig initial_playback,
                         float configured_gain)
    : bridge_{bridge}, img_{img}, configured_gain_{configured_gain},
      mixer_pause_delay_ms_{std::clamp(initial_playback.radio_pause_delay_ms, 20, 5000)},
      game_state_{img},
      playback_opts_{std::make_shared<const PlaybackConfig>(std::move(initial_playback))},
      thread_{[this](const std::stop_token& tok) { run(tok); }} {}

void ControlLoop::push_playback_options(PlaybackConfig opts) {
    mixer_pause_delay_ms_.store(std::clamp(opts.radio_pause_delay_ms, 20, 5000),
                                std::memory_order_release);
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

    bool acquired = false;
    for (int attempt = 0; attempt < kDiscoveryTries && !tok.stop_requested(); ++attempt) {
        if (acquire_target()) {
            acquired = true;
            break;
        }
        for (auto t = std::chrono::steady_clock::now() + kDiscoveryRetry;
             std::chrono::steady_clock::now() < t && !tok.stop_requested();)
            std::this_thread::sleep_for(kTick);
    }
    if (!acquired) {
        log::warn("[ctrl] discovery timed out; control loop exiting");
        return;
    }

    // The radio HUD reads from the SampleProperties slots at a much lower
    // rate than the audio mixer. 4 Hz is more than enough and keeps the
    // memory writes off the hot path.
    constexpr int kMetaEveryNTicks = 12; // ~240 ms at the 20 ms tick rate.
    int meta_tick                  = 0;

    auto next = std::chrono::steady_clock::now();
    while (!tok.stop_requested()) {
        next += kTick;
        const auto loop_now = std::chrono::steady_clock::now();
        if (bridge_.retarget_if_needed()) last_retarget_ = loop_now;
        const std::uint64_t c = bridge_.call_count();
        if (c != prev_calls_) {
            mixer_idle_ticks_ = 0;
            retune_attempts_ = 0;
            retune_suppressed_ = false;
        } else {
            ++mixer_idle_ticks_;
        }
        const int pause_ms = mixer_pause_delay_ms_.load(std::memory_order_acquire);
        const int pause_ticks =
            std::max(1, (pause_ms + static_cast<int>(kTick.count()) - 1) /
                            static_cast<int>(kTick.count()));
        mixer_consuming_ = mixer_idle_ticks_ < pause_ticks;

        run_playback_state_machines(loop_now);
        bridge_.manager().pump_once();

        if (++meta_tick >= kMetaEveryNTicks) {
            meta_tick = 0;
            push_metadata();
        }

        if (bridge_.diagnostics_enabled() && ++diagnostics_tick_ >= 50) {
            diagnostics_tick_ = 0;
            const auto calls = bridge_.call_count();
            const auto delta = calls - prev_diagnostics_calls_;
            prev_diagnostics_calls_ = calls;
            log::info("[dspdiag] radio_active={} mixer={} calls/s={} input_peak={:.3f} "
                      "ring={} underruns={}",
                      bridge_.radio_active(), mixer_consuming_, delta,
                      bridge_.consume_input_peak_milli() / 1000.0f,
                      bridge_.manager().ring().readable(), bridge_.underruns());
        }

        // Staleness watchdog: while a source is actively producing audio,
        // FMOD's mixer should be invoking our read_callback every tick. If
        // call_count() freezes for ~1s, the game tore down the radio channel
        // and won't rebuild it on its own. Toggling the in-game station off
        // and back on is the only thing that makes it allocate a fresh
        // channel; retarget_if_needed() then re-attaches the DSP next tick.
        // Gated on R10 so we never yank the user off a station they chose.
        auto* active          = bridge_.manager().active();
        const bool has_pcm    = bridge_.manager().ring().readable() > 0;
        const bool busy       = active && has_pcm &&
                          (active->playback_state() == PlaybackState::playing ||
                           active->playback_state() == PlaybackState::buffering);
        if (busy && c == prev_calls_) {
            if (++stale_ticks_ >= kStaleTickThreshold) {
                stale_ticks_   = 0;
                const auto now = std::chrono::steady_clock::now();
                auto disc = discover_radio_instances(img_);
                const RadioInstance* current = select_instance(disc);
                const bool channel_dead =
                    !current || !bridge_.channel_handle_alive(current->radio_stream);
                if (retune_attempts_ >= kMaxRetuneAttemptsPerStall) {
                    if (!retune_suppressed_) {
                        retune_suppressed_ = true;
                        log::warn("[ctrl] radio recovery suppressed after {} stalled retune attempts",
                                  retune_attempts_);
                    }
                } else if (channel_dead &&
                           now - last_retune_ >= kRetuneCooldown &&
                           game_state_.read().on_target_station &&
                           game_state_.retune_streamer_station()) {
                    last_retune_ = now;
                    ++retune_attempts_;
                    // The toggle may hand us a freshly-allocated RadioStreamFmod;
                    // re-point at the live one so retarget_if_needed installs there.
                    acquire_target();
                }
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

bool ControlLoop::acquire_target() noexcept {
    auto disc                   = discover_radio_instances(img_);
    const RadioInstance* chosen = select_instance(disc);
    if (!chosen) return false;
    if (chosen->sound_name != kTargetSoundName)
        log::info(R"([ctrl] using custom radio carrier "{}" instead of preferred "{}")",
                  chosen->sound_name, kTargetSoundName);

    void* fmod_system = resolve_fmod_system(img_, chosen->radio_stream);
    if (!fmod_system) {
        log::warn("[ctrl] FMOD SystemI resolution failed");
        return false;
    }
    bridge_.set_target(*chosen, fmod_system);
    meta_.set_target(chosen->sample_props_body);
    log::info("[ctrl] targeting RadioStreamFmod @0x{:X} SoundName=\"{}\" title_cap={} artist_cap={} SystemI*=0x{:X}",
              reinterpret_cast<uintptr_t>(chosen->radio_stream), chosen->sound_name,
              chosen->title_capacity, chosen->artist_capacity, reinterpret_cast<uintptr_t>(fmod_system));
    return true;
}

const RadioInstance* ControlLoop::select_instance(const DiscoveryResult& disc) const noexcept {
    const RadioInstance* best_live     = nullptr;  // live R9 carrier with largest HUD strings
    const RadioInstance* best_fallback = nullptr;  // same scoring, but any handle state
    std::uint64_t best_live_score      = 0;
    std::uint64_t best_fallback_score  = 0;
    for (auto& i : disc.instances) {
        const bool is_target = i.sound_name == kTargetSoundName;
        const bool is_custom = i.sound_name.rfind(kCustomStationPrefix, 0) == 0;
        if (!is_custom) continue;
        const auto hud_cap = std::min(i.title_capacity, i.artist_capacity);
        const auto score = hud_cap * 10 + (is_target ? 1 : 0);
        const bool live = bridge_.channel_handle_alive(i.radio_stream);
        if (live && (!best_live || score > best_live_score)) {
            best_live = &i;
            best_live_score = score;
        }
        if (!best_fallback || score > best_fallback_score) {
            best_fallback = &i;
            best_fallback_score = score;
        }
    }
    if (!best_live && !best_fallback && !disc.instances.empty()) {
        log::warn("[ctrl] found radio stream(s), but none are R9 custom carriers; rescanning");
        reset_radio_discovery_cache();
    }
    return best_live ? best_live : best_fallback;
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

    std::shared_ptr<const PlaybackConfig> opts;
    {
        std::lock_guard lock{playback_opts_mtx_};
        opts = playback_opts_;
    }
    if (!opts) return;
    auto* active = bridge_.manager().active();
    if (!active) {
        prev_r10_ = prev_race_ = prev_race_restart_ = false;
        have_prev_r10_ = false;
        prev_radio_active_ = false;
        have_prev_radio_active_ = false;
        prev_dsp_active_ = false;
        have_prev_dsp_active_ = false;
        transport_active_ = false;
        have_transport_active_ = false;
        raw_transport_active_ = false;
        have_raw_transport_active_ = false;
        prev_raw_transport_flip_ = {};
        active_source_ = nullptr;
        bridge_.set_radio_active(false);
        quick_skip_armed_ = false;
        return;
    }
    if (active->consume_drain_request()) {
        bridge_.manager().ring().drain();
        meta_.reset_cache();
        log::info("[ctrl] source requested ring drain");
    }

    const auto game = game_state_.read();
    // R10 = "user is currently tuned to our station" via FH6 game state, NOT
    // FMOD channel aliveness. FMOD flaps the channel during race scene
    // transitions even though the user stayed on our station, which used to
    // trip a phantom quickStationSkip on every race start.
    const bool r10 = game.on_target_station;
    const bool mixer_known = bridge_.call_count() > 0;
    const bool retarget_settling =
        r10 && !mixer_consuming_ && last_retarget_ != time_point{} &&
        now - last_retarget_ <= kRetargetTransportGrace;
    const bool dsp_active = r10 && (!mixer_known || mixer_consuming_ || retarget_settling);
    auto& ring     = bridge_.manager().ring();
    const bool source_changed = active != active_source_;
    if (source_changed) {
        active_source_ = active;
        have_transport_active_ = false;
        have_raw_transport_active_ = false;
        prev_raw_transport_flip_ = {};
    }

    if (!have_prev_dsp_active_ || source_changed) {
        have_prev_dsp_active_ = true;
        prev_dsp_active_ = dsp_active;
        meta_.reset_cache();
        bridge_.set_radio_active(dsp_active);
        active->on_audio_sink_active_changed(dsp_active);
        if (!dsp_active) ring.drain();
    } else if (dsp_active != prev_dsp_active_) {
        log::info("[ctrl] dsp {} (station={}, mixer={})",
                  dsp_active ? "active" : "inactive", r10, mixer_consuming_);
        meta_.reset_cache();
        bridge_.set_radio_active(dsp_active);
        active->on_audio_sink_active_changed(dsp_active);
        ring.drain();
        prev_dsp_active_ = dsp_active;
    }

    const bool desired_transport_active = dsp_active;
    if (!have_raw_transport_active_) {
        have_raw_transport_active_ = true;
        raw_transport_active_ = desired_transport_active;
        last_raw_transport_flip_ = now;
    } else if (desired_transport_active != raw_transport_active_) {
        prev_raw_transport_flip_ = last_raw_transport_flip_;
        raw_transport_active_ = desired_transport_active;
        last_raw_transport_flip_ = now;
    }

    if (!have_transport_active_) {
        have_transport_active_ = true;
        transport_active_ = desired_transport_active;
        last_transport_change_ = now;
        active->on_radio_audible(transport_active_);
    } else if (desired_transport_active != transport_active_) {
        const bool repeated_flips =
            prev_raw_transport_flip_ != time_point{} &&
            last_raw_transport_flip_ - prev_raw_transport_flip_ <= kTransportJitterWindow;
        const bool calm_enough =
            !repeated_flips || now - last_raw_transport_flip_ >= kTransportQuietPeriod;
        if (calm_enough && now - last_transport_change_ >= kTransportMinToggleGap) {
            transport_active_ = desired_transport_active;
            last_transport_change_ = now;
            log::info("[ctrl] radio {} (station={}, mixer={})",
                      transport_active_ ? "active" : "inactive", r10, mixer_consuming_);
            active->on_radio_audible(transport_active_);
        }
    }

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
    std::shared_ptr<const PlaybackConfig> opts;
    {
        std::lock_guard lock{playback_opts_mtx_};
        opts = playback_opts_;
    }
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
    if (opts && opts->show_album_in_hud && !info.album.empty()) {
        artist = artist.empty() ? info.album : artist + " - " + info.album;
    }
    if (artist.empty()) {
        switch (a->playback_state()) {
            case PlaybackState::playing:   artist = "Playing"; break;
            case PlaybackState::buffering: artist = "Buffering"; break;
            case PlaybackState::paused:    artist = "Paused"; break;
            case PlaybackState::stopped:   artist = "Stopped"; break;
        }
    }
    const bool ok = meta_.update(title, artist);
    bridge_.set_metadata_status(std::move(title), std::move(artist), ok);
}

} // namespace fh6::fmod_bridge
