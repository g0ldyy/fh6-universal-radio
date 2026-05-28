#pragma once

#include "fh6/config.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>

namespace fh6 {

// ReplayGain clean_max: gain_lin = 10^(dB/20),
// clamped against (peak * gain_lin) with a 0.88 (-1.1 dB) headroom budget.
// Returns the multiplier to apply per sample. coef==1.0f means identity.
inline float compute_loudness_correction(float gain_db, float peak_lin,
                                         float pre_gain = 1.0f) noexcept {
    if (!std::isfinite(gain_db) || !std::isfinite(peak_lin) || peak_lin <= 0.0f || pre_gain < 1.0f)
        return 1.0f;
    const float lin      = std::pow(10.0f, gain_db / 20.0f);
    const float headroom = 0.88f / (peak_lin * lin);
    return std::min(headroom, std::max(lin, pre_gain));
}

// Scan a file (FLAC/OGG/Opus Vorbis comments, MP3 ID3v2 TXXX) for
// REPLAYGAIN_TRACK_GAIN / REPLAYGAIN_TRACK_PEAK. out_gain_db/out_peak left
// untouched (still NaN) if not found. Reads at most 256 KiB.
void parse_replaygain_file(const std::filesystem::path& path, float& out_gain_db,
                           float& out_peak_lin) noexcept;

// 5-band peaking RBJ biquad. Designed to live on the producer side -- coefs
// are recomputed only when set_options() bumps the version (called from the
// config on_change hook), so process() is allocation-free and read-mostly.
class EqualizerStage {
public:
    static constexpr std::size_t kBands = 5;

    void set_options(bool enabled, const std::array<float, kBands>& bands, float sample_rate_hz) {
        std::scoped_lock lk{mu_};
        pending_.enabled = enabled;
        pending_.bands   = bands;
        pending_.fs      = sample_rate_hz;
        version_.fetch_add(1, std::memory_order_release);
    }

    void process(int16_t* samples, std::size_t frames) noexcept {
        static constexpr std::array<float, kBands> kCentres = {60.0f, 250.0f, 1000.0f, 4000.0f,
                                                               12000.0f};

        if (version_.load(std::memory_order_acquire) != applied_version_) {
            std::scoped_lock lk{mu_};
            active_          = pending_;
            applied_version_ = version_.load(std::memory_order_relaxed);
            for (std::size_t i = 0; i < kBands; ++i) {
                set_peaking(filters_[i], active_.fs, kCentres[i], active_.bands[i]);
                filters_[i].z1l = filters_[i].z2l = 0.0f;
                filters_[i].z1r = filters_[i].z2r = 0.0f;
            }
        }
        if (!active_.enabled) return;

        for (std::size_t i = 0; i < frames; ++i) {
            float l = static_cast<float>(samples[2 * i + 0]) * (1.0f / 32768.0f);
            float r = static_cast<float>(samples[2 * i + 1]) * (1.0f / 32768.0f);
            for (auto& f : filters_) {
                l = process_one(f, l, f.z1l, f.z2l);
                r = process_one(f, r, f.z1r, f.z2r);
            }
            samples[2 * i + 0] = clamp16(l);
            samples[2 * i + 1] = clamp16(r);
        }
    }

private:
    struct Biquad {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float z1l = 0, z2l = 0, z1r = 0, z2r = 0;
    };
    struct Snap {
        bool enabled = false;
        std::array<float, kBands> bands{};
        float fs = 48000.0f;
    };

    static void set_peaking(Biquad& f, float fs, float f0, float gain_db,
                            float Q = 1.4142f) noexcept {
        const float A     = std::pow(10.0f, gain_db / 40.0f);
        const float w0    = 2.0f * 3.14159265358979323846f * f0 / fs;
        const float cw    = std::cos(w0);
        const float sw    = std::sin(w0);
        const float alpha = sw / (2.0f * Q);
        const float a0    = 1.0f + alpha / A;
        f.b0              = (1.0f + alpha * A) / a0;
        f.b1              = (-2.0f * cw) / a0;
        f.b2              = (1.0f - alpha * A) / a0;
        f.a1              = (-2.0f * cw) / a0;
        f.a2              = (1.0f - alpha / A) / a0;
    }
    static float process_one(const Biquad& f, float x, float& z1, float& z2) noexcept {
        const float y = f.b0 * x + z1;
        z1            = f.b1 * x - f.a1 * y + z2;
        z2            = f.b2 * x - f.a2 * y;
        return y;
    }
    static int16_t clamp16(float v) noexcept {
        v *= 32768.0f;
        if (v > 32767.0f) return 32767;
        if (v < -32768.0f) return -32768;
        return static_cast<int16_t>(v);
    }

    std::mutex mu_;
    std::atomic<uint64_t> version_{0};
    uint64_t applied_version_ = ~uint64_t{0};
    Snap pending_;
    Snap active_;
    std::array<Biquad, kBands> filters_{};
};

} // namespace fh6
