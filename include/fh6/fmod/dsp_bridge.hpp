#pragma once

#include "fh6/fmod/pe_image.hpp"
#include "fh6/fmod/radio_discovery.hpp"
#include "fh6/ring_buffer.hpp"

#include <atomic>
#include <cstdint>

namespace fh6 {
class AudioSourceManager;
} // namespace fh6

namespace fh6::fmod_bridge {

enum class DSPMode : uint32_t { off = 0, passthrough = 1, silence = 2, pcm = 3 };

// FMOD trampolines resolved from the game image (stdcall, C linkage).
struct FMODFns {
    using SystemCreateDSP_t = uint32_t (*)(void* system, const void* desc, void** out);
    using DSPRelease_t      = uint32_t (*)(void* dsp);

    // ChannelControl* arg is actually FMOD's packed 32-bit handle widened
    // to 64 bits (`(void*)(uint64_t)handle`). Resolver returns the real
    // pointer, but addDSP/removeDSP both want the handle.
    using ChannelControlAddDSP_t  = uint32_t (*)(uint64_t channel_handle, int32_t index, void* dsp);
    using ChannelControlRemDSP_t  = uint32_t (*)(uint64_t channel_handle, void* dsp);
    using ChannelControlSetMode_t = uint32_t (*)(uint64_t channel_handle, uint32_t mode);

    // Handle::open. Third out param is __int64 in the reference; declaring
    // it uint32_t* would risk a 4-byte stack overrun.
    using HandleResolver_t = uint32_t (*)(uint32_t handle, void** out_inst, uint64_t* out_kind);
    // Handle::unlock. Must pair every open or the handle table leaks a
    // slot and the game thread eventually freezes contending on it.
    using HandleUnlock_t = uint32_t (*)(uint64_t lock_state);

    SystemCreateDSP_t system_create_dsp              = nullptr;
    DSPRelease_t dsp_release                         = nullptr;
    ChannelControlAddDSP_t channel_control_add_dsp   = nullptr;
    ChannelControlRemDSP_t channel_control_rem_dsp   = nullptr;
    ChannelControlSetMode_t channel_control_set_mode = nullptr;
    HandleResolver_t handle_resolver                 = nullptr;
    HandleUnlock_t handle_unlock                     = nullptr;

    // Game module base, kept so the bridge can re-scan for createDSP at
    // install time if the LEA wasn't resident at DLL load.
    std::byte* host_base = nullptr;

    // system_create_dsp is lazy-resolved on first install. handle_unlock and
    // channel_control_set_mode are best-effort: install proceeds without
    // them. Missing unlock leaks resolver slots; missing set_mode means the
    // channel can die when the placeholder sample's natural duration elapses
    // (Forza won't always allocate a new one).
    bool ready() const noexcept {
        return host_base && dsp_release && channel_control_add_dsp &&
               channel_control_rem_dsp && handle_resolver;
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
    void retarget_if_needed() noexcept;

    // True while our currently-installed channel handle is still resolvable.
    // Goes false when FMOD destroys the channel without writing a fresh
    // handle to +0x20 (e.g. the placeholder sample reached its natural end).
    bool current_handle_alive() const noexcept;

    // True when `radio_stream`+0x20 holds a live FMOD channel handle. Used
    // by the control loop to pick a recovery candidate after staleness.
    bool channel_handle_alive(std::byte* radio_stream) const noexcept;

    DSPMode mode() const noexcept { return mode_.load(std::memory_order_acquire); }
    void set_mode(DSPMode m) noexcept;

    // [0, 1]. Pure linear multiplier on the S16->float conversion; 1.0 is
    // unity (bit-perfect).
    float gain() const noexcept { return gain_.load(std::memory_order_acquire); }
    void set_gain(float g) noexcept { gain_.store(g, std::memory_order_release); }

    uint64_t underruns() const noexcept { return underruns_.load(std::memory_order_relaxed); }
    uint64_t call_count() const noexcept { return calls_.load(std::memory_order_relaxed); }
    uint32_t last_buffer_len() const noexcept { return last_len_.load(std::memory_order_relaxed); }
    uint32_t last_out_channels() const noexcept {
        return last_out_ch_.load(std::memory_order_relaxed);
    }

    AudioSourceManager& manager() noexcept { return mgr_; }

    static uint32_t __stdcall read_callback(void* dsp_state, float* in_buf, float* out_buf,
                                            uint32_t length, int32_t in_channels,
                                            int32_t* out_channels);

private:
    // True if the resolver accepts the handle (the channel is still live).
    bool validate_handle(uint32_t handle) const noexcept;
    // Returns the live channel handle at radio_stream+0x20, or 0 if absent
    // or dead. Centralises the FMOD read+validate that retarget and the
    // public `*_alive` queries both need.
    uint32_t read_live_handle(std::byte* radio_stream) const noexcept;
    void release_current_dsp_locked() noexcept;
    void install_dsp_locked(uint32_t handle) noexcept;

    AudioSourceManager& mgr_;
    FMODFns fns_;

    void* fmod_system_       = nullptr;
    void* current_dsp_       = nullptr;
    uint32_t current_handle_ = 0;
    mutable uint32_t last_bad_handle_ = 0;  // suppress repeated rc=3 / SEH warnings for the same handle
    std::byte* radio_stream_ = nullptr;

    std::atomic<DSPMode> mode_{DSPMode::pcm};
    std::atomic<float> gain_{1.0f};

    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint64_t> calls_{0};
    std::atomic<uint32_t> last_len_{0};
    std::atomic<uint32_t> last_out_ch_{0};
};

} // namespace fh6::fmod_bridge
