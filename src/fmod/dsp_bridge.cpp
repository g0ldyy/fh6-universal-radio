#include "fh6/fmod/dsp_bridge.hpp"
#include "fh6/fmod/sig_scanner.hpp"
#include "fh6/audio_source_manager.hpp"
#include "fh6/log.hpp"
#include "fh6/safe_mem.hpp"

#include <algorithm>
#include <cstring>

namespace fh6::fmod_bridge {

namespace {

struct FMODSig {
    const char* anchor;
    const char* pattern;
};

// FMOD entry points we resolve from Forza's statically-linked FMOD build.
// Anchors are FMOD's own "Class::method" strings baked into .rdata; patterns
// are the x64 MSVC prologues FMOD has shipped throughout the 1.x line.
constexpr FMODSig kAnchored[] = {
    {"System::createDSP", "4C 8B DC 56 48 81 EC 70 01 00 00|"
                          "40 53 55 56 57 41 56 48 81 EC 50 01 00 00"},
    {"DSP::release", "48 89 5C 24 10 57 48 81 EC 50 01 00 00"},
    {"ChannelControl::addDSP", "4C 8B DC 56 48 81 EC 70 01 00 00|"
                               "40 53 55 56 57 41 56 48 81 EC 50 01 00 00"},
    {"ChannelControl::removeDSP", "48 89 5C 24 18 48 89 74 24 20 57 48 81 EC 50 01 00 00"},
    // setMode is best-effort -- tries every FMOD prologue we know; install
    // proceeds without it. Used to force FMOD_LOOP_NORMAL on the radio
    // channel so the placeholder sample never reaches its natural end.
    {"ChannelControl::setMode", "4C 8B DC 56 48 81 EC 70 01 00 00|"
                                "40 53 55 56 57 41 56 48 81 EC 50 01 00 00|"
                                "48 89 5C 24 10 57 48 81 EC 50 01 00 00|"
                                "48 89 5C 24 18 48 89 74 24 20 57 48 81 EC 50 01 00 00|"
                                "40 53 48 83 EC 20|"
                                "48 89 5C 24 08 57 48 83 EC 20"},
};

// FMOD_LOOP_NORMAL: makes the channel loop forever on its source sample.
// Set once at install time so the placeholder sample doesn't end and
// drop the channel out from under our DSP.
constexpr uint32_t kFmodLoopNormal = 0x2;

// FMOD's `Handle::open` / `Handle::unlock` have no .rdata anchor; we match
// their (unique) prologues directly.
constexpr const char* kResolverPattern =
    "48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 20 8B F9 "
    "8B C1 C1 EF 11 49 8B F0 D1 E8 81 E7 FF 0F 00 00 0F B7 E8 4C 8B "
    "F2 4C 8B F9";
constexpr const char* kUnlockPattern = "48 8B 89 F0 09 01 00 48 85 C9 0F 85 ?? ?? ?? ?? 33 C0 C3";

DSPBridge* g_bridge = nullptr;

// FMOD Studio Core API DSP descriptor (216 bytes). Zero-valued fields are
// treated as "unprovided", so we only fill what we use.
struct FMOD_DSP_DESCRIPTION {
    uint32_t pluginsdkversion; //   0
    char name[32];             //   4
    uint32_t version;          //  36
    int32_t numinputbuffers;   //  40
    int32_t numoutputbuffers;  //  44
    void* create;              //  48
    void* release;             //  56
    void* reset;               //  64
    void* read;                //  72  <- our callback
    void* process;             //  80
    void* setposition;         //  88
    int32_t numparameters;     //  96 (+4 padding)
    void* paramdesc;           // 104
    void* setparamfloat;       // 112
    void* setparamint;         // 120
    void* setparambool;        // 128
    void* setparamdata;        // 136
    void* getparamfloat;       // 144
    void* getparamint;         // 152
    void* getparambool;        // 160
    void* getparamdata;        // 168
    void* shouldiprocess;      // 176
    void* userdata;            // 184  <- bridge pointer
    void* sys_register;        // 192
    void* sys_deregister;      // 200
    void* sys_mix;             // 208
};
static_assert(sizeof(FMOD_DSP_DESCRIPTION) == 216);

// The `System::createDSP` LEA is sometimes not yet resident in the host
// process when our scanner runs at DllMain. Pulled out so install_dsp_locked
// can retry against a fresh parse of the same module.
FMODFns::SystemCreateDSP_t resolve_create_dsp(const PEImage& img) noexcept {
    return reinterpret_cast<FMODFns::SystemCreateDSP_t>(
        find_by_anchor(img, kAnchored[0].anchor, kAnchored[0].pattern));
}

} // namespace

bool resolve_fmod_signatures(const PEImage& img, FMODFns& out) noexcept {
    if (!img.valid()) return false;

    out.host_base         = img.base;
    out.system_create_dsp = resolve_create_dsp(img);
    out.dsp_release       = reinterpret_cast<FMODFns::DSPRelease_t>(
        find_by_anchor(img, kAnchored[1].anchor, kAnchored[1].pattern));
    out.channel_control_add_dsp = reinterpret_cast<FMODFns::ChannelControlAddDSP_t>(
        find_by_anchor(img, kAnchored[2].anchor, kAnchored[2].pattern));
    out.channel_control_rem_dsp = reinterpret_cast<FMODFns::ChannelControlRemDSP_t>(
        find_by_anchor(img, kAnchored[3].anchor, kAnchored[3].pattern));
    out.channel_control_set_mode = reinterpret_cast<FMODFns::ChannelControlSetMode_t>(
        find_by_anchor(img, kAnchored[4].anchor, kAnchored[4].pattern));
    out.handle_resolver =
        reinterpret_cast<FMODFns::HandleResolver_t>(find_by_pattern(img, kResolverPattern));
    out.handle_unlock =
        reinterpret_cast<FMODFns::HandleUnlock_t>(find_by_pattern(img, kUnlockPattern));

    log::info("[sigscan] createDSP={} dsp_release={} addDSP={} removeDSP={} setMode={} "
              "resolver={} unlock={}",
              (void*)out.system_create_dsp, (void*)out.dsp_release,
              (void*)out.channel_control_add_dsp, (void*)out.channel_control_rem_dsp,
              (void*)out.channel_control_set_mode, (void*)out.handle_resolver,
              (void*)out.handle_unlock);

    if (!out.system_create_dsp) {
        log::info("[sigscan] System::createDSP not yet visible -- will retry at first install");
    }
    if (!out.handle_unlock) {
        log::warn("[sigscan] Handle::unlock not resolved -- the resolver lock will leak; "
                  "expect the game to freeze a few seconds after DSP install");
    }
    if (!out.channel_control_set_mode) {
        log::warn("[sigscan] ChannelControl::setMode not resolved -- the radio channel "
                  "will die at the placeholder sample's end and the user will have to "
                  "toggle the in-game radio to recover");
    }
    return out.ready();
}

DSPBridge::DSPBridge(AudioSourceManager& mgr, const FMODFns& fns) : mgr_{mgr}, fns_{fns} {
    g_bridge = this;
}

DSPBridge::~DSPBridge() {
    release_current_dsp_locked();
    if (g_bridge == this) g_bridge = nullptr;
}

void DSPBridge::set_mode(DSPMode m) noexcept {
    auto prev = mode_.exchange(m, std::memory_order_acq_rel);
    if (prev != m) log::info("[dsp] mode {} -> {}", (int)prev, (int)m);
}

bool DSPBridge::validate_handle(uint32_t handle) const noexcept {
    if (!handle) return false;
    void* inst          = nullptr;
    uint64_t lock_state = 0;
    uint32_t rc         = ~0u;
    if (!seh_call([&] { rc = fns_.handle_resolver(handle, &inst, &lock_state); })) {
        log::warn("[dsp] handle_resolver raised SEH exception");
        return false;
    }
    // Handle::open must always be paired with Handle::unlock, even on rc!=0.
    // Skipping it leaks an FMOD handle slot and eventually freezes the game
    // thread.
    if (fns_.handle_unlock && lock_state) {
        seh_call([&] { fns_.handle_unlock(lock_state); });
    }
    if (rc != 0) {
        log::warn("[dsp] handle_resolver rc={}", rc);
        return false;
    }
    return inst != nullptr;
}

void DSPBridge::release_current_dsp_locked() noexcept {
    if (!current_dsp_) return;
    if (current_handle_)
        seh_call([&] { fns_.channel_control_rem_dsp(current_handle_, current_dsp_); });
    seh_call([&] { fns_.dsp_release(current_dsp_); });
    current_dsp_    = nullptr;
    current_handle_ = 0;
}

void DSPBridge::install_dsp_locked(uint32_t handle) noexcept {
    if (!fmod_system_ || !handle) return;

    // Lazy-resolve createDSP. The first install runs long after FMOD is up
    // (control loop discovery has to finish), so the LEA that DllMain-time
    // sigscan sometimes misses is reliably present here.
    if (!fns_.system_create_dsp && fns_.host_base) {
        fns_.system_create_dsp = resolve_create_dsp(parse(fns_.host_base));
        if (fns_.system_create_dsp) {
            log::info("[dsp] resolved System::createDSP late at {}",
                      (void*)fns_.system_create_dsp);
        } else {
            log::warn("[dsp] System::createDSP still unresolved -- install aborted");
            return;
        }
    }

    // createDSP rejects a wrong plugin SDK stamp; we try all three FMOD
    // shipped across the 1.x line and keep the first that takes.
    constexpr uint32_t kVersions[] = {0x00011000u, 0x00011003u, 0x00010000u};

    FMOD_DSP_DESCRIPTION desc{};
    std::memcpy(desc.name, "FH6 Universal Radio", 19);
    desc.version          = 1;
    desc.numinputbuffers  = 1;
    desc.numoutputbuffers = 1;
    desc.read             = reinterpret_cast<void*>(&DSPBridge::read_callback);
    desc.userdata         = this;

    void* dsp   = nullptr;
    uint32_t rc = ~0u;
    for (uint32_t v : kVersions) {
        desc.pluginsdkversion = v;
        if (!seh_call([&] { rc = fns_.system_create_dsp(fmod_system_, &desc, &dsp); })) {
            log::warn("[dsp] createDSP raised SEH (sdkver=0x{:X})", v);
            dsp = nullptr;
            continue;
        }
        if (rc == 0 && dsp) break;
        dsp = nullptr;
    }
    if (!dsp) {
        log::warn("[dsp] createDSP failed r={}", rc);
        return;
    }

    // addDSP wants the packed handle zero-extended to 64 bits.
    const auto channel = static_cast<uint64_t>(handle);
    if (!seh_call([&] { rc = fns_.channel_control_add_dsp(channel, 0, dsp); }) || rc != 0) {
        log::warn("[dsp] addDSP failed r={}", rc);
        seh_call([&] { fns_.dsp_release(dsp); });
        return;
    }

    current_dsp_    = dsp;
    current_handle_ = handle;
    log::info("[dsp] installed dsp={} on handle=0x{:X}", dsp, handle);

    // Pin the channel in loop mode so FMOD doesn't tear it down when the
    // placeholder sample reaches its natural end. Without this, FMOD drops
    // the channel after ~2 min and Forza only allocates a replacement
    // handle when the user toggles the in-game radio.
    if (fns_.channel_control_set_mode) {
        uint32_t mrc = ~0u;
        if (!seh_call([&] { mrc = fns_.channel_control_set_mode(channel, kFmodLoopNormal); }) ||
            mrc != 0) {
            log::warn("[dsp] setMode(FMOD_LOOP_NORMAL) failed r={}; channel may die early", mrc);
        }
    }
}

void DSPBridge::set_target(const RadioInstance& inst, void* fmod_system) noexcept {
    fmod_system_  = fmod_system;
    radio_stream_ = inst.radio_stream;
}

uint32_t DSPBridge::read_live_handle(std::byte* radio_stream) const noexcept {
    if (!radio_stream || !fns_.ready()) return 0;
    // Active FMOD Channel handle sits at +0x20 of the inline RadioStreamFmod.
    uint32_t handle = 0;
    if (!safe_read(radio_stream + 0x20, handle) || !handle) return 0;
    return validate_handle(handle) ? handle : 0;
}

bool DSPBridge::current_handle_alive() const noexcept {
    return current_handle_ != 0 && fns_.ready() && validate_handle(current_handle_);
}

bool DSPBridge::channel_handle_alive(std::byte* radio_stream) const noexcept {
    return read_live_handle(radio_stream) != 0;
}

void DSPBridge::retarget_if_needed() noexcept {
    if (mode() != DSPMode::pcm || !fmod_system_) return;
    const uint32_t handle = read_live_handle(radio_stream_);
    if (!handle || handle == current_handle_) return;

    log::info("[dsp] retargeting -> handle 0x{:X}", handle);
    release_current_dsp_locked();
    install_dsp_locked(handle);
}

// FMOD DSP read callback (mixer thread). Sources write 48 kHz S16 stereo
// (miniaudio / ffmpeg resample upstream), which is FMOD's master rate, so
// the callback is a straight int16 -> float conversion with gain.
uint32_t __stdcall DSPBridge::read_callback(void* /*dsp_state*/, float* in_buf, float* out_buf,
                                            uint32_t length, int32_t in_channels,
                                            int32_t* out_channels) {
    auto* b = g_bridge;
    if (!b || !out_buf) return 0;
    const DSPMode m = b->mode();

    // Use only what FMOD allocated: out_buf is pre-sized by FMOD, writing
    // more channels than requested is a heap overflow that crashes the mixer
    // a few seconds later. If FMOD wants mono, downmix our stereo.
    int32_t out_ch = in_channels > 0 ? in_channels : 2;
    if (out_channels && *out_channels > 0) out_ch = *out_channels;
    if (out_channels) *out_channels = out_ch;
    const std::size_t total = static_cast<std::size_t>(length) * out_ch;

    auto stats = [&] {
        b->calls_.fetch_add(1, std::memory_order_relaxed);
        b->last_len_.store(length, std::memory_order_relaxed);
        b->last_out_ch_.store(out_ch, std::memory_order_relaxed);
    };

    if (m == DSPMode::silence || m == DSPMode::off) {
        std::memset(out_buf, 0, total * sizeof(float));
        stats();
        return 0;
    }
    if (m == DSPMode::passthrough) {
        if (in_buf) std::memcpy(out_buf, in_buf, total * sizeof(float));
        else        std::memset(out_buf, 0, total * sizeof(float));
        stats();
        return 0;
    }

    // PCM mode. Pre-zero so a mid-callback underrun leaves silence in the
    // tail, not the stale floats FMOD handed us.
    std::memset(out_buf, 0, total * sizeof(float));

    const float gain = b->gain();
    if (gain <= 0.0f) {
        stats();
        return 0;
    }

    // gain<=1.0 + S16 input keeps the float strictly within [-1, 1], but a
    // misconfigured config.toml could push gain above 1 -- clamp defensively.
    constexpr float kAmp = 1.0f / 32768.0f;
    const float scale    = gain * kAmp;

    // Pull the ring in chunks: one ring.read() per chunk amortises the
    // ring's two atomic loads over many frames. 1024 frames = 4 KiB stack,
    // well under any sane FMOD callback budget.
    constexpr uint32_t kChunkFrames = 1024;
    int16_t scratch[kChunkFrames * 2];
    auto& ring = b->mgr_.ring();

    for (uint32_t produced = 0; produced < length;) {
        const uint32_t want_frames = std::min(length - produced, kChunkFrames);
        const std::size_t got      = ring.read(scratch, want_frames * 4);
        const uint32_t got_frames  = static_cast<uint32_t>(got / 4);

        for (uint32_t f = 0; f < got_frames; ++f) {
            const float fl = scratch[f * 2 + 0] * scale;
            const float fr = scratch[f * 2 + 1] * scale;
            const float L  = fl > 1.0f ? 1.0f : (fl < -1.0f ? -1.0f : fl);
            const float R  = fr > 1.0f ? 1.0f : (fr < -1.0f ? -1.0f : fr);

            float* o = out_buf + static_cast<std::size_t>(produced + f) * out_ch;
            if (out_ch == 1) {
                o[0] = (L + R) * 0.5f;
            } else {
                o[0]           = L;
                o[1]           = R;
                const float dn = (L + R) * 0.5f;
                for (int32_t c = 2; c < out_ch; ++c) o[c] = dn;
            }
        }

        produced += got_frames;
        if (got_frames < want_frames) {
            b->underruns_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }

    stats();
    return 0;
}

} // namespace fh6::fmod_bridge
