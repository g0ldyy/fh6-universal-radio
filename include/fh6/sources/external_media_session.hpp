#pragma once

#include "fh6/audio_source.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fh6::sources {

struct ExternalAudioMediaSession {
    std::string id;
    std::string name;
    bool is_current = false;
    bool is_selected = false;
};

bool external_audio_media_sessions_available() noexcept;
std::vector<ExternalAudioMediaSession>
enumerate_external_audio_media_sessions(std::string_view selected_id = {});

std::optional<TrackInfo> external_audio_media_session_track(std::string_view selected_id,
                                                            uint64_t fallback_position_ms = 0);
bool external_audio_media_session_next(std::string_view selected_id);
bool external_audio_media_session_previous(std::string_view selected_id);
bool external_audio_media_session_pause(std::string_view selected_id);
bool external_audio_media_session_play(std::string_view selected_id);

} // namespace fh6::sources
