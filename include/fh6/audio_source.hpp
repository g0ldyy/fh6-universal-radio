#pragma once

#include "fh6/ring_buffer.hpp"

#include <cstdint>
#include <string>

namespace fh6 {

struct PlaybackConfig;

struct TrackInfo {
    std::string title;
    std::string artist;
    std::string album;
    std::string artwork_url;
    uint64_t duration_ms = 0;
    uint64_t position_ms = 0;
};

enum class PlaybackState { stopped, playing, paused, buffering };
enum class AuthState { none_required, authenticated, needs_auth, error };

struct SourceCapabilities {
    bool seek     = false;
    bool previous = false;
    bool queue    = false;
};

// One instance per provider; the AudioSourceManager owns them all and at
// most one is active. Only the active source's PCM reaches the game.
class IAudioSource {
public:
    virtual ~IAudioSource() = default;

    virtual std::string_view name() const noexcept         = 0;
    virtual std::string_view display_name() const noexcept = 0;

    virtual bool initialize()        = 0;
    virtual void shutdown() noexcept = 0;

    virtual void play()      = 0;
    virtual void pause()     = 0;
    virtual void stop()      = 0;
    virtual void next() {}
    virtual void previous() {}
    virtual void seek(uint64_t /*ms*/) {}

    // ControlLoop's quickStationSkip / raceStartPlayback="next" entry.
    // Returns true iff the queue actually advanced. Default delegates to
    // next() and assumes success.
    virtual bool skip_next() { next(); return true; }

    // ControlLoop's raceStartPlayback="restart" entry. Returns true iff
    // playback actually rewound to t=0. Default is "unsupported".
    virtual bool restart_current() { return false; }

    // Pull PCM into the ring. Sources that push from their own thread no-op.
    virtual void pump(RingBuffer&) {}

    // Fan-out from ConfigStore::on_change: producer-side ReplayGain coef and
    // EQ band/enable updates land here. No-op for sources that don't care.
    virtual void set_playback_options(const PlaybackConfig& /*opts*/) {}

    virtual TrackInfo current_track() const               = 0;
    virtual PlaybackState playback_state() const noexcept = 0;
    virtual AuthState auth_state() const noexcept         = 0;
    virtual std::string auth_instructions() const { return {}; }
    virtual SourceCapabilities capabilities() const noexcept = 0;
};

} // namespace fh6
