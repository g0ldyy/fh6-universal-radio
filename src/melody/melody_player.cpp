// miniaudio.h — implementation is defined once in local_file_source.cpp
#include "miniaudio.h"

#include "fh6/melody/melody_player.hpp"
#include "fh6/log.hpp"

#include <windows.h> // GetTickCount64

#include <cstdlib>   // rand, srand
#include <ctime>     // time
#include <string>

namespace fh6::melody {

// ---------------------------------------------------------------------------
// Backend preference order for Wine/Proton compatibility.
// WinMM is the most reliable backend under Wine; try it first, then fall back.
// ---------------------------------------------------------------------------
static const ma_backend kPreferredBackends[] = {
    ma_backend_winmm,
    ma_backend_wasapi,
    ma_backend_dsound,
};

// Try to initialise a miniaudio engine.
// Returns the engine pointer on success, nullptr on failure.
// When a custom context is created and succeeds, it is written to *out_ctx
// so the caller can free it on cleanup (stored as own_context_).
static ma_engine* try_init_engine(ma_context** out_ctx) {
    *out_ctx = nullptr;
    auto* eng = new ma_engine{};

    // -----------------------------------------------------------------------
    // Attempt 1: explicit backend list (WinMM first) via a custom context
    // -----------------------------------------------------------------------
    auto* ctx = new ma_context{};
    ma_context_config ctx_cfg = ma_context_config_init();
    ma_result r = ma_context_init(kPreferredBackends,
                                  static_cast<ma_uint32>(std::size(kPreferredBackends)),
                                  &ctx_cfg, ctx);
    if (r == MA_SUCCESS) {
        ma_engine_config eng_cfg = ma_engine_config_init();
        eng_cfg.pContext = ctx;
        if (ma_engine_init(&eng_cfg, eng) == MA_SUCCESS) {
            log::info("[melody] miniaudio engine ready (WinMM-preferred context)");
            *out_ctx = ctx; // caller owns this context
            return eng;
        }
        ma_context_uninit(ctx);
    }
    delete ctx;

    // -----------------------------------------------------------------------
    // Attempt 2: fully automatic backend selection (miniaudio default)
    // -----------------------------------------------------------------------
    ma_engine_config eng_cfg2 = ma_engine_config_init();
    if (ma_engine_init(&eng_cfg2, eng) == MA_SUCCESS) {
        log::info("[melody] miniaudio engine ready (auto backend)");
        return eng; // *out_ctx stays nullptr — no owned context
    }

    log::warn("[melody] miniaudio engine init failed (both attempts); "
              "will retry on next trigger");
    delete eng;
    return nullptr;
}

// ---------------------------------------------------------------------------
// MelodyPlayer
// ---------------------------------------------------------------------------

MelodyPlayer::MelodyPlayer(std::filesystem::path base_dir)
    : base_dir_{std::move(base_dir)} {
    std::srand(static_cast<unsigned>(std::time(nullptr)));
    // Engine is NOT initialised here intentionally.
    // Wine/Proton audio subsystem may not be ready when the DLL first loads.
    // Lazy init happens on the first trigger() call instead.
}

MelodyPlayer::~MelodyPlayer() {
    if (engine_) {
        ma_engine_uninit(static_cast<ma_engine*>(engine_));
        delete static_cast<ma_engine*>(engine_);
    }
    if (context_) {
        ma_context_uninit(static_cast<ma_context*>(context_));
        delete static_cast<ma_context*>(context_);
    }
}

void MelodyPlayer::trigger(const StartupMelodyConfig& cfg) {
    if (!cfg.enabled) return;

    // -----------------------------------------------------------------------
    // Lazy-initialise the audio engine.
    // Throttle retry attempts to once per kInitRetryIntervalMs.
    // -----------------------------------------------------------------------
    if (!engine_) {
        const unsigned long long now_pre = GetTickCount64();
        if (last_init_attempt_ms_ != 0 &&
            now_pre - last_init_attempt_ms_ < kInitRetryIntervalMs) {
            return; // too soon to retry
        }
        last_init_attempt_ms_ = now_pre;

        ma_context* ctx = nullptr;
        ma_engine*  eng = try_init_engine(&ctx);
        if (!eng) return; // still not ready
        engine_  = eng;
        context_ = ctx; // may be nullptr if auto-init succeeded
    }

    // -----------------------------------------------------------------------
    // Debounce check
    // -----------------------------------------------------------------------
    const unsigned long long now = GetTickCount64();
    if (cfg.debounce_ms > 0 &&
        now - last_play_ms_ < static_cast<unsigned long long>(cfg.debounce_ms)) {
        return;
    }

    // -----------------------------------------------------------------------
    // Scan folder and pick a random file
    // -----------------------------------------------------------------------
    auto files = scan_melody_files(base_dir_);
    if (files.empty()) {
        log::warn("[melody] no audio files found in: {}", base_dir_.string());
        return;
    }

    const int idx = std::rand() % static_cast<int>(files.size());
    const std::filesystem::path chosen = base_dir_ / files[static_cast<size_t>(idx)];
    const std::string path_str = chosen.string();

    // -----------------------------------------------------------------------
    // Play asynchronously via miniaudio engine (fire-and-forget)
    // -----------------------------------------------------------------------
    ma_engine* eng = static_cast<ma_engine*>(engine_);
    if (ma_engine_play_sound(eng, path_str.c_str(), nullptr) == MA_SUCCESS) {
        last_play_ms_ = now;
        log::info("[melody] playing ({}/{}): {}", idx + 1,
                  static_cast<int>(files.size()), files[static_cast<size_t>(idx)]);
    } else {
        log::warn("[melody] playback failed for: {} — resetting engine", path_str);
        // Tear down so we re-initialise cleanly on the next trigger
        ma_engine_uninit(eng);
        delete eng;
        engine_ = nullptr;
        if (context_) {
            ma_context_uninit(static_cast<ma_context*>(context_));
            delete static_cast<ma_context*>(context_);
            context_ = nullptr;
        }
        last_init_attempt_ms_ = 0; // allow immediate retry next time
    }
}

} // namespace fh6::melody
