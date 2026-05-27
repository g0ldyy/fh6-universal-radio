#include "fh6/sources/roon_source.hpp"

#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error{message};
}

void require_contains(const std::string& text, const char* needle, const char* message) {
    if (text.find(needle) == std::string::npos) throw std::runtime_error{message};
}

} // namespace

int main() {
    fh6::RoonConfig disabled_cfg;
    fh6::sources::RoonSource disabled{disabled_cfg};
    require(!disabled.initialize(), "disabled Roon source should not initialize");

    fh6::RoonConfig cfg;
    cfg.enabled = true;
    fh6::sources::RoonSource source{cfg};

    require(source.name() == "roon", "name should be roon");
    require(source.display_name() == "Roon", "display name should be Roon");
    require(source.initialize(), "enabled placeholder source should initialize");
    require(source.playback_state() == fh6::PlaybackState::stopped, "initial state should stop");
    require(source.auth_state() == fh6::AuthState::needs_auth, "incomplete setup should need auth");
    require_contains(source.auth_instructions(), "Authorize", "instructions should mention authorize");
    require_contains(source.auth_instructions(), "zone", "instructions should mention zone setup");
    require_contains(source.auth_instructions(), "capture", "instructions should mention capture setup");

    const auto caps = source.capabilities();
    require(!caps.seek, "placeholder should not advertise seek");
    require(caps.previous, "placeholder should support previous");
    require(!caps.queue, "placeholder should not advertise queue");

    auto track = source.current_track();
    require(track.title.empty(), "placeholder current track title should be empty");
    require(track.artist.empty(), "placeholder current track artist should be empty");

    source.play();
    require(source.playback_state() == fh6::PlaybackState::playing, "play should cache playing state");
    source.pause();
    require(source.playback_state() == fh6::PlaybackState::paused, "pause should cache paused state");
    source.next();
    require(source.playback_state() == fh6::PlaybackState::playing, "next should resume placeholder");
    source.previous();
    require(source.playback_state() == fh6::PlaybackState::playing, "previous should resume placeholder");
    source.stop();
    require(source.playback_state() == fh6::PlaybackState::stopped, "stop should cache stopped state");

    fh6::RingBuffer ring{4096};
    source.pump(ring);
    require(ring.readable() == 0, "placeholder pump should not write PCM");

    source.shutdown();
    source.shutdown();
    require(source.playback_state() == fh6::PlaybackState::stopped, "shutdown should be idempotent");

    return 0;
}
