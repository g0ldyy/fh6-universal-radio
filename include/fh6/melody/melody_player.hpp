#pragma once

#include "fh6/config.hpp"
#include "fh6/melody/melody_scanner.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace fh6::melody {

/// Plays a randomly chosen audio file from the melodies folder using miniaudio.
/// Supports WAV, MP3, FLAC, OGG, Opus, M4A. Uses a debounce window so rapid
/// triggers (race start, car switch) never stack up overlapping audio.
///
/// The miniaudio engine is lazy-initialised on the first trigger() call rather
/// than in the constructor, which makes it robust under Wine/Proton where the
/// audio subsystem may not be ready when the DLL loads.
class MelodyPlayer {
public:
    /// base_dir – directory containing the audio files
    ///            e.g. "<mod_root>/fh6-radio/assets/melodies/"
    explicit MelodyPlayer(std::filesystem::path base_dir);
    ~MelodyPlayer();

    MelodyPlayer(const MelodyPlayer&)            = delete;
    MelodyPlayer& operator=(const MelodyPlayer&) = delete;

    /// Rescans the folder and picks a random file, then plays it.
    /// No-op when cfg.enabled is false, folder is empty, or debounce is active.
    /// Lazily initialises the miniaudio engine on first call.
    void trigger(const StartupMelodyConfig& cfg);

private:
    std::filesystem::path base_dir_;
    unsigned long long    last_play_ms_         = 0;

    // Lazy engine initialisation state
    void*              engine_               = nullptr; // ma_engine*
    void*              context_              = nullptr; // ma_context* (may be nullptr)
    unsigned long long last_init_attempt_ms_ = 0;

    // Retry engine init at most once per 30 seconds to avoid hammering audio
    static constexpr unsigned long long kInitRetryIntervalMs = 30'000ULL;
};

} // namespace fh6::melody
