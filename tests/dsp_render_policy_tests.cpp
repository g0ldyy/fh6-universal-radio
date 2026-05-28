#include "fh6/fmod/dsp_render_policy.hpp"

#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error{message};
}

bool near(float actual, float expected, float tolerance = 0.0001f) {
    return std::fabs(actual - expected) <= tolerance;
}

} // namespace

int main() {
    using namespace fh6::fmod_bridge;

    DspOutputFormat format{};
    force_stereo_output_format(format);
    require(format.channels == 2, "DSP process query should request stereo output");
    require(format.channel_mask == kFmodChannelMaskStereo,
            "DSP process query should use the stereo channel mask");
    require(format.speaker_mode == kFmodSpeakerModeStereo,
            "DSP process query should use stereo speaker mode");
    require(kFmodDspProcessPerform == 0, "FMOD process perform op must match the SDK ABI");
    require(kFmodDspProcessQuery == 1, "FMOD process query op must match the SDK ABI");

    float stereo[2]{};
    render_s16_stereo_frame(16384, -16384, 1.0f, stereo, 2);
    require(near(stereo[0], 0.5f), "stereo render should preserve left channel");
    require(near(stereo[1], -0.5f), "stereo render should preserve right channel");

    float mono[1]{};
    render_s16_stereo_frame(16384, -8192, 1.0f, mono, 1);
    require(near(mono[0], 0.1767767f),
            "mono carrier fallback should use equal-power stereo downmix");

    float clamped[1]{};
    render_s16_stereo_frame(32767, 32767, 1.0f, clamped, 1);
    require(clamped[0] <= 1.0f, "mono downmix should clamp positive overflow");

    return 0;
}
