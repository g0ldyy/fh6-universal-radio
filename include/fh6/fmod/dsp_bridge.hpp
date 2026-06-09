#pragma once

#include "fh6/fmod/pe_image.hpp"
#include "fh6/fmod/radio_discovery.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace fh6 {
class AudioSourceManager;
} // namespace fh6

namespace fh6::fmod_bridge {

enum class DSPMode : uint32_t { off = 0, passthrough = 1, silence = 2, pcm = 3 };

// FMOD trampolines resolved from the game image (stdcall, C linkage).
struct FMODFns {
    using SystemCreateDSP_t = uint32_t (*)(void* system, const void* desc, void** out);
    using DSPRelease_t      = uint32_t (*)(void* dsp);

    using ChannelControlAddDSP_t = uint32_t (*)(uint64_t channel_handle, int32_t index, void* dsp);
    using ChannelControlRemDSP_t = uint32_t (*)(uint64_t channel_handle, void* dsp);

    using HandleResolver_t = uint32_t (*)(uint32_t handle, void** out_inst, uint64_t* out_kind);
    // Handle::unlock. Must pair every open or the handle table leaks a
    // slot and the game thread eventually freezes contending on it.
    using HandleUnlock_t = uint32_t (*)(uint64_t lock_state);

    SystemCreateDSP_t system_create_dsp            = nullptr;
    DSPRelease_t dsp_release                       = nullptr;
    ChannelControlAddDSP_t channel_control_add_dsp = nullptr;
    ChannelControlRemDSP_t channel_control_rem_dsp = nullptr;
    HandleResolver_t handle_resolver               = nullptr;
    HandleUnlock_t handle_unlock                   = nullptr;

    // Game module base, kept so the bridge can re-scan for createDSP at
    // install time if the LEA wasn't resident at DLL load.
    std::byte* host_base = nullptr;

    // system_create_dsp is lazy-resolved on first install. handle_unlock is
    // best-effort: install proceeds without it (missing unlock just leaks
    // resolver slots).
    bool ready() const noexcept {
        return host_base && dsp_release && channel_control_add_dsp && channel_control_rem_dsp &&
               handle_resolver;
    }
};

bool resolve_fmod_signatures(const PEImage& img, FMODFns& out) noexcept;

// Holds the FMOD DSP handle and the read callback that feeds PCM from the
// AudioSourceManager's ring buffer into FMOD's mixer. Pinned as a global so
// the C-linkage callback can find it.
class DSPBridge {
public:
    DSPBridge(AudioSourceManager& mgr, const FMODFns& fns);
    ~DSPBridge();

    DSPBridge(const DSPBridge&)            = delete;
    DSPBridge& operator=(const DSPBridge&) = delete;

    void set_target(const RadioInstance& inst, void* fmod_system) noexcept;

    // Re-attach if the game stored a new channel handle on the RadioStream
    // (station changed, race ended, etc.). Cheap to call every tick.
    bool retarget_if_needed() noexcept;

    // True when `radio_stream`+0x20 holds a live, resolvable FMOD channel
    // handle. Lets the control loop prefer the instance actually carrying
    // audio when several share the placeholder SoundName.
    bool channel_handle_alive(std::byte* radio_stream) const noexcept {
        return read_live_handle(radio_stream) != 0;
    }

    DSPMode mode() const noexcept { return mode_.load(std::memory_order_acquire); }
    void set_mode(DSPMode m) noexcept;

    // [0, 1]. Pure linear multiplier on the S16->float conversion; 1.0 is
    // unity (bit-perfect).
    float gain() const noexcept { return gain_.load(std::memory_order_acquire); }
    void set_gain(float g) noexcept { gain_.store(g, std::memory_order_release); }

    bool force_stereo_audio() const noexcept {
        return force_stereo_audio_.load(std::memory_order_acquire);
    }
    // Only steers the read callback's channel count (1 = mono, 2 = stereo);
    // FMOD re-queries *out_channels every callback, so no channel-mode touch.
    void set_force_stereo_audio(bool v) noexcept {
        force_stereo_audio_.store(v, std::memory_order_release);
    }

    bool radio_active() const noexcept { return radio_active_.load(std::memory_order_acquire); }
    void set_radio_active(bool v) noexcept {
        radio_active_.store(v, std::memory_order_release);
    }

    bool diagnostics_enabled() const noexcept {
        return diagnostics_enabled_.load(std::memory_order_acquire);
    }
    void set_diagnostics_enabled(bool v) noexcept {
        diagnostics_enabled_.store(v, std::memory_order_release);
    }
    uint32_t consume_input_peak_milli() noexcept {
        return input_peak_milli_.exchange(0, std::memory_order_acq_rel);
    }

    uint64_t underruns() const noexcept { return underruns_.load(std::memory_order_relaxed); }
    uint64_t call_count() const noexcept { return calls_.load(std::memory_order_relaxed); }
    uint32_t last_buffer_len() const noexcept { return last_len_.load(std::memory_order_relaxed); }
    uint32_t last_out_channels() const noexcept {
        return last_out_ch_.load(std::memory_order_relaxed);
    }

    struct MetadataStatus {
        std::string title;
        std::string artist;
        bool ok = false;
        uint64_t updates = 0;
    };

    void set_metadata_status(std::string title, std::string artist, bool ok);
    MetadataStatus metadata_status() const;

    AudioSourceManager& manager() noexcept { return mgr_; }

    static uint32_t __stdcall read_callback(void* dsp_state, float* in_buf, float* out_buf,
                                            uint32_t length, int32_t in_channels,
                                            int32_t* out_channels);

private:
    // True if the resolver accepts the handle (the channel is still live).
    bool validate_handle(uint32_t handle) const noexcept;
    // Returns the live channel handle at radio_stream+0x20, or 0 if absent
    // or dead, validating it through the resolver.
    uint32_t read_live_handle(std::byte* radio_stream) const noexcept;
    void release_current_dsp_locked() noexcept;
    bool install_dsp_locked(uint32_t handle) noexcept;

    AudioSourceManager& mgr_;
    FMODFns fns_;

    void* fmod_system_ = nullptr;
    void* current_dsp_ = nullptr;
    // Channel handle the DSP is installed on. Mutated from the control-loop
    // thread via install/release/retarget; read from the same thread.
    std::atomic<uint32_t> current_handle_{0};
    mutable uint32_t last_bad_handle_ =
        0; // suppress repeated rc=3 / SEH warnings for the same handle
    std::byte* radio_stream_ = nullptr;

    std::atomic<DSPMode> mode_{DSPMode::pcm};
    std::atomic<float> gain_{1.0f};
    std::atomic<bool> force_stereo_audio_{false};
    std::atomic<bool> radio_active_{false};
    std::atomic<bool> diagnostics_enabled_{false};
    std::atomic<uint32_t> input_peak_milli_{0};

    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint64_t> calls_{0};
    std::atomic<uint32_t> last_len_{0};
    std::atomic<uint32_t> last_out_ch_{0};

    mutable std::mutex metadata_mtx_;
    MetadataStatus metadata_status_;
};

} // namespace fh6::fmod_bridge
