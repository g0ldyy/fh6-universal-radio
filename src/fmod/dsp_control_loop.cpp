#include <windows.h>
#include <xinput.h>
#include "fh6/fmod/dsp_control_loop.hpp"
#include "fh6/fmod/radio_discovery.hpp"
#include "fh6/audio_source.hpp"
#include "fh6/audio_source_manager.hpp"
#include "fh6/log.hpp"

#include <chrono>
#include <bit>

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
                         float configured_gain, std::function<bool()> on_cycle_station)
    : bridge_{bridge}, img_{img}, configured_gain_{configured_gain}, game_state_{img},
      on_cycle_station_{std::move(on_cycle_station)},
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

    // --- Hotkeys ---
    // dynamically load XInput
    typedef DWORD(WINAPI* XInputGetState_t)(DWORD, XINPUT_STATE*);
    static XInputGetState_t pXInputGetState = nullptr;
    static std::once_flag xinput_once;
    std::call_once(xinput_once, [] {
        HMODULE hXInput = LoadLibraryW(L"xinput1_4.dll");
        if (!hXInput) hXInput = LoadLibraryW(L"xinput9_1_0.dll");
        if (!hXInput) hXInput = LoadLibraryW(L"xinput1_3.dll");
        if (hXInput) {
            pXInputGetState =
                reinterpret_cast<XInputGetState_t>(GetProcAddress(hXInput, "XInputGetState"));
        }
    });

    XINPUT_STATE xstate{};
    const bool has_pad_hotkeys = opts->hotkeys.pad_skip || opts->hotkeys.pad_source || opts->hotkeys.pad_playpause || opts->hotkeys.pad_prev || opts->hotkeys.pad_next_station;
    bool pad_connected = has_pad_hotkeys && pXInputGetState && (pXInputGetState(0, &xstate) == ERROR_SUCCESS);

    // helper to resolve overlap conflicts
    auto check_pad = [&](int mask) {
        return pad_connected && mask && (mask != 0x9999) && ((xstate.Gamepad.wButtons & mask) == mask);
    };

    // check keyboard (direct)
    // helper function to decode keyboard combinations
    auto check_kb = [](int bind) {
        if (!bind || bind == 0x9999) return false;
        
        int vk = bind & 0xFF;
        bool shift_req = (bind & 0x0100) != 0;
        bool ctrl_req  = (bind & 0x0200) != 0;
        bool alt_req   = (bind & 0x0400) != 0;

        // check if the base key is physically pressed
        if (!(GetAsyncKeyState(vk) & 0x8000)) return false;

        // strictly check if the modifiers match the requirement
        bool shift_down = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        bool ctrl_down  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool alt_down   = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

        return (shift_req == shift_down) && (ctrl_req == ctrl_down) && (alt_req == alt_down);
    };

    // evaluate keyboard inputs
    bool kb_skip    = check_kb(opts->hotkeys.kb_skip);
    bool kb_src     = check_kb(opts->hotkeys.kb_source);
    bool kb_pp      = check_kb(opts->hotkeys.kb_playpause);
    bool kb_prev    = check_kb(opts->hotkeys.kb_prev);
    bool kb_station = check_kb(opts->hotkeys.kb_next_station);

    // check controller (resolve overlap by complexity)
    bool p_skip = check_pad(opts->hotkeys.pad_skip);
    bool p_src  = check_pad(opts->hotkeys.pad_source);
    bool p_pp   = check_pad(opts->hotkeys.pad_playpause);
    bool p_prev = check_pad(opts->hotkeys.pad_prev);
    bool p_station  = check_pad(opts->hotkeys.pad_next_station);

    DWORD max_bits = std::max<DWORD>({
        p_skip ? static_cast<DWORD>(std::popcount(static_cast<uint32_t>(opts->hotkeys.pad_skip))) : 0u,
        p_src  ? static_cast<DWORD>(std::popcount(static_cast<uint32_t>(opts->hotkeys.pad_source))) : 0u,
        p_pp   ? static_cast<DWORD>(std::popcount(static_cast<uint32_t>(opts->hotkeys.pad_playpause))) : 0u,
        p_prev ? static_cast<DWORD>(std::popcount(static_cast<uint32_t>(opts->hotkeys.pad_prev))) : 0u,
        p_station ? static_cast<DWORD>(std::popcount(static_cast<uint32_t>(opts->hotkeys.pad_next_station))) : 0u
    });

    bool pad_skip_pressed = p_skip && (static_cast<DWORD>(std::popcount(static_cast<uint32_t>(opts->hotkeys.pad_skip))) == max_bits);
    bool pad_src_pressed  = p_src && (static_cast<DWORD>(std::popcount(static_cast<uint32_t>(opts->hotkeys.pad_source))) == max_bits);
    bool pad_pp_pressed   = p_pp && (static_cast<DWORD>(std::popcount(static_cast<uint32_t>(opts->hotkeys.pad_playpause))) == max_bits);
    bool pad_prev_pressed = p_prev && (static_cast<DWORD>(std::popcount(static_cast<uint32_t>(opts->hotkeys.pad_prev))) == max_bits);
    bool pad_station_pressed = p_station && (static_cast<DWORD>(std::popcount(static_cast<uint32_t>(opts->hotkeys.pad_next_station))) == max_bits);

    bool skip_pressed = kb_skip || pad_skip_pressed;
    bool src_pressed  = kb_src || pad_src_pressed;
    bool pp_pressed   = kb_pp || pad_pp_pressed;
    bool prev_pressed   = kb_prev || pad_prev_pressed;
    bool station_pressed     = kb_station || pad_station_pressed;

    // --- quickStationSkip (R10 edge) ---
    const bool use_old_skip = (opts->hotkeys.kb_skip      == 0x9999 || opts->hotkeys.pad_skip      == 0x9999);
    const bool use_old_src  = (opts->hotkeys.kb_source    == 0x9999 || opts->hotkeys.pad_source    == 0x9999);
    const bool use_old_pp   = (opts->hotkeys.kb_playpause == 0x9999 || opts->hotkeys.pad_playpause == 0x9999);
    const bool use_old_prev = (opts->hotkeys.kb_prev      == 0x9999 || opts->hotkeys.pad_prev      == 0x9999);
    const bool use_old_station = (opts->hotkeys.kb_next_station == 0x9999 || opts->hotkeys.pad_next_station == 0x9999);
    
    if (prev_r10_ && !r10) {
        last_r10_off_ = now;
        if (use_old_skip || use_old_src || use_old_pp || use_old_prev || use_old_station) quick_skip_armed_ = true;
    } else if (!prev_r10_ && r10) {
        if (quick_skip_armed_ && now - last_r10_off_ <= kQuickSkipWindow) {
            if (use_old_skip) old_method_skip_fired_ = true;
            if (use_old_src)  old_method_src_fired_  = true;
            if (use_old_pp)   old_method_pp_fired_   = true;
            if (use_old_prev) old_method_prev_fired_ = true;
            if (use_old_station) old_method_station_fired_ = true;
        }
        quick_skip_armed_ = false;
    }
    prev_r10_ = r10;

    // check for rising edges (button just pressed)
    bool skip_edge = skip_pressed && !prev_skip_hotkey_;
    bool src_edge  = src_pressed && !prev_source_hotkey_;
    bool pp_edge   = pp_pressed && !prev_playpause_hotkey_;
    bool prev_edge = prev_pressed && !prev_prev_hotkey_;
    bool station_edge = station_pressed && !prev_station_hotkey_;

    bool trigger_skip = old_method_skip_fired_;
    bool trigger_src  = old_method_src_fired_;
    bool trigger_pp   = old_method_pp_fired_;
    bool trigger_prev = old_method_prev_fired_;
    bool trigger_station = old_method_station_fired_;
    old_method_skip_fired_ = false;
    old_method_src_fired_  = false;
    old_method_pp_fired_   = false;
    old_method_prev_fired_ = false;
    old_method_station_fired_ = false;

    // buffer controller inputs or fire keyboard immediately
    if (skip_edge) {
        if (kb_skip) trigger_skip = true;
        else pending_skip_ = true;
    }
    if (src_edge) {
        if (kb_src) trigger_src = true;
        else pending_src_ = true;
    }
    if (pp_edge) {
        if (kb_pp) trigger_pp = true;
        else pending_pp_ = true;
    }
    if (prev_edge) {
        if (kb_prev) trigger_prev = true;
        else pending_prev_ = true;
    }
    if (station_edge) {
        if (kb_station) trigger_station = true;
        else pending_station_ = true;
    }

    // track how long buttons have been held to allow combos to form
    if (pad_skip_pressed || pad_src_pressed || pad_pp_pressed || pad_prev_pressed || pad_station_pressed) {
        if (combo_wait_ticks_ >= 0) {
            combo_wait_ticks_++;
        }
    } else {
        combo_wait_ticks_ = 0;
        pending_skip_ = pending_src_ = pending_pp_ = pending_prev_ = pending_station_ = false;
    }

    // trigger combos immediately, delay single buttons by 3 ticks (~60ms) to prevent overlap
    if (combo_wait_ticks_ == 3 || (max_bits > 1 && combo_wait_ticks_ > 0)) {
        if (pending_pp_ && max_bits == static_cast<DWORD>(std::popcount(static_cast<uint32_t>(opts->hotkeys.pad_playpause)))) {
            trigger_pp = true;
        } else if (pending_src_ && max_bits == static_cast<DWORD>(std::popcount(static_cast<uint32_t>(opts->hotkeys.pad_source)))) {
            trigger_src = true;
        } else if (pending_skip_ && max_bits == static_cast<DWORD>(std::popcount(static_cast<uint32_t>(opts->hotkeys.pad_skip)))) {
            trigger_skip = true;
        } else if (pending_prev_ && max_bits == static_cast<DWORD>(std::popcount(static_cast<uint32_t>(opts->hotkeys.pad_prev)))) {
            trigger_prev = true;
        } else if (pending_station_ && max_bits == static_cast<DWORD>(std::popcount(static_cast<uint32_t>(opts->hotkeys.pad_next_station)))) {
            trigger_station = true;
        }

        // clear pending states
        pending_skip_ = pending_src_ = pending_pp_ = pending_prev_ = pending_station_ = false;
        
        // set to a negative number to prevent firing again until buttons are released
        combo_wait_ticks_ = -9999; 
    }

    // execute skip track
    if (trigger_skip && (now - last_skip_cmd_ >= kSkipCommandCooldown)) {
        if (active->skip_next()) {
            ring.drain();
            last_skip_cmd_ = now;
            log::info("[ctrl] Hotkey triggered: advanced to next track");
        }
    }
    
    // execute source switch
    if (trigger_src && (now - last_source_cmd_ >= 250ms)) {
        auto sources = bridge_.manager().sources_snapshot();
        if (!sources.empty()) {
            std::size_t next_idx = 0;
            for (std::size_t i = 0; i < sources.size(); ++i) {
                if (sources[i] == active) { next_idx = (i + 1) % sources.size(); break; }
            }
            bridge_.manager().switch_to(sources[next_idx]->name());
            ring.drain();
            last_source_cmd_ = now;
            log::info("[ctrl] Hotkey triggered: switched source to {}", sources[next_idx]->name());
        }
    }

    // execute play/pause toggle
    if (trigger_pp && (now - last_playpause_cmd_ >= 250ms)) {
        auto state = active->playback_state();
        if (state == PlaybackState::playing || state == PlaybackState::buffering) {
            active->pause();
            log::info("[ctrl] Hotkey triggered: paused playback");
        } else {
            active->play();
            log::info("[ctrl] Hotkey triggered: resumed playback");
        }
        last_playpause_cmd_ = now;
    }

    // execute previous track
    if (trigger_prev && (now - last_prev_cmd_ >= kSkipCommandCooldown)) {
        active->previous();
        ring.drain();
        last_prev_cmd_ = now;
        log::info("[ctrl] Hotkey triggered: returned to previous track");
    }

    // execute cycle station/playlist
    if (trigger_station && (now - last_station_cmd_ >= 250ms)) {
        if (on_cycle_station_ && on_cycle_station_()) {
            ring.drain();
            log::info("[ctrl] Hotkey triggered: cycled active station");
        }
        last_station_cmd_ = now;
    }

    prev_skip_hotkey_ = skip_pressed;
    prev_source_hotkey_ = src_pressed;
    prev_playpause_hotkey_ = pp_pressed;
    prev_prev_hotkey_ = prev_pressed;
    prev_station_hotkey_ = station_pressed;
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
