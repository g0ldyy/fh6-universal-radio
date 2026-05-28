#include "fh6/audio_source_manager.hpp"
#include "fh6/fmod/dsp_bridge.hpp"
#include "fh6/fmod/dsp_render_policy.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error{message};
}

int g_fake_dsp     = 0;
int g_fake_channel = 0;

struct TestFmodDspBufferArray {
    int32_t numbuffers;
    int32_t* buffernumchannels;
    uint32_t* bufferchannelmask;
    float** buffers;
    int32_t speakermode;
};

uint32_t fake_create_dsp(void*, const void*, void** out) {
    *out = &g_fake_dsp;
    return 0;
}

uint32_t fake_release_dsp(void*) { return 0; }

uint32_t fake_add_dsp(uint64_t, int32_t, void*) { return 0; }

uint32_t fake_remove_dsp(uint64_t, void*) { return 0; }

uint32_t fake_set_mode(uint64_t, uint32_t) { return 0; }

uint32_t fake_resolve_handle(uint32_t, void** out, uint64_t* lock_state) {
    *out        = &g_fake_channel;
    *lock_state = 0;
    return 0;
}

uint32_t fake_unlock(uint64_t) { return 0; }

fh6::fmod_bridge::FMODFns fake_fmod_functions() {
    fh6::fmod_bridge::FMODFns fns;
    fns.system_create_dsp        = fake_create_dsp;
    fns.dsp_release              = fake_release_dsp;
    fns.channel_control_add_dsp  = fake_add_dsp;
    fns.channel_control_rem_dsp  = fake_remove_dsp;
    fns.channel_control_set_mode = fake_set_mode;
    fns.handle_resolver          = fake_resolve_handle;
    fns.handle_unlock            = fake_unlock;
    fns.host_base                = reinterpret_cast<std::byte*>(0x1);
    return fns;
}

} // namespace

int main() {
    fh6::AudioSourceManager mgr{4096};
    fh6::fmod_bridge::DSPBridge bridge{mgr, fake_fmod_functions()};

    std::array<std::byte, 0x28> radio_stream{};
    const uint32_t live_handle = 0x1234u;
    std::memcpy(radio_stream.data() + 0x20, &live_handle, sizeof(live_handle));

    fh6::fmod_bridge::RadioInstance inst{};
    inst.radio_stream = radio_stream.data();
    bridge.set_target(inst, reinterpret_cast<void*>(0x2));

    const std::array<std::byte, 32> stale_pcm{};
    require(mgr.ring().write(stale_pcm.data(), stale_pcm.size()) == stale_pcm.size(),
            "test should seed stale PCM before DSP install");
    require(mgr.ring().readable() == stale_pcm.size(), "stale PCM should be queued");

    bridge.retarget_if_needed();

    require(bridge.current_handle_alive(), "retarget should install a live DSP handle");
    require(mgr.ring().readable() == 0, "installing a new DSP should drain stale PCM");

    int32_t in_channels[1]{2};
    uint32_t in_mask[1]{fh6::fmod_bridge::kFmodChannelMaskStereo};
    float in_samples[4]{0.25f, -0.25f, 0.5f, -0.5f};
    float* in_buffers[1]{in_samples};
    TestFmodDspBufferArray in_array{1, in_channels, in_mask, in_buffers,
                                    fh6::fmod_bridge::kFmodSpeakerModeStereo};

    int32_t out_channels[1]{2};
    uint32_t out_mask[1]{0};
    float out_samples[4]{};
    float* out_buffers[1]{out_samples};
    TestFmodDspBufferArray out_array{1, out_channels, out_mask, out_buffers, 0};

    bridge.set_mode(fh6::fmod_bridge::DSPMode::passthrough);
    fh6::fmod_bridge::DSPBridge::process_callback(nullptr, 2, &in_array, &out_array, false,
                                                  fh6::fmod_bridge::kFmodDspProcessPerform);

    require(out_channels[0] == 2, "process perform should keep stereo output");
    require(out_samples[0] == in_samples[0] && out_samples[1] == in_samples[1],
            "passthrough process should copy the first stereo frame");
    require(out_samples[2] == in_samples[2] && out_samples[3] == in_samples[3],
            "passthrough process should copy the second stereo frame");
    return 0;
}
