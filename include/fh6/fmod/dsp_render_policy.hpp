#pragma once

#include <algorithm>
#include <cstdint>

namespace fh6::fmod_bridge {

constexpr uint32_t kFmodChannelMaskStereo = 0x00000003u;
constexpr int32_t kFmodSpeakerModeStereo  = 3;
constexpr uint32_t kFmodDspProcessPerform = 0;
constexpr uint32_t kFmodDspProcessQuery   = 1;
constexpr float kS16ToFloatScale          = 1.0f / 32768.0f;
constexpr float kMonoEqualPowerGain       = 0.70710678118f;

struct DspOutputFormat {
    int32_t channels      = 0;
    uint32_t channel_mask = 0;
    int32_t speaker_mode  = 0;
};

inline void force_stereo_output_format(DspOutputFormat& format) noexcept {
    format.channels     = 2;
    format.channel_mask = kFmodChannelMaskStereo;
    format.speaker_mode = kFmodSpeakerModeStereo;
}

inline float clamp_unit(float value) noexcept { return std::clamp(value, -1.0f, 1.0f); }

inline void render_s16_stereo_frame(int16_t left, int16_t right, float gain, float* out,
                                    int32_t out_channels) noexcept {
    if (!out || out_channels <= 0) return;

    const float scale = gain * kS16ToFloatScale;
    const float l     = clamp_unit(static_cast<float>(left) * scale);
    const float r     = clamp_unit(static_cast<float>(right) * scale);

    if (out_channels == 1) {
        out[0] = clamp_unit((l + r) * kMonoEqualPowerGain);
        return;
    }

    out[0]           = l;
    out[1]           = r;
    const float mono = clamp_unit((l + r) * 0.5f);
    for (int32_t c = 2; c < out_channels; ++c) out[c] = mono;
}

} // namespace fh6::fmod_bridge
