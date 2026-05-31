#include "fh6/sources/external_media_session.hpp"

#include "fh6/log.hpp"

#include <algorithm>
#include <chrono>

#if __has_include(<winrt/Windows.Media.Control.h>) && \
    __has_include(<winrt/Windows.Foundation.Collections.h>)
#define FH6_EXTERNAL_AUDIO_HAS_CPPWINRT 1
#include <roapi.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#else
#define FH6_EXTERNAL_AUDIO_HAS_CPPWINRT 0
#endif

namespace fh6::sources {
namespace {

#if FH6_EXTERNAL_AUDIO_HAS_CPPWINRT
namespace media = winrt::Windows::Media::Control;

struct RoApartment {
    bool uninit = false;

    RoApartment() noexcept {
        const HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
        uninit = SUCCEEDED(hr);
    }

    ~RoApartment() {
        if (uninit) RoUninitialize();
    }
};

std::string s(winrt::hstring const& value) {
    return winrt::to_string(value);
}

uint64_t ms_from_timespan(winrt::Windows::Foundation::TimeSpan const& ts) noexcept {
    using namespace std::chrono;
    const auto ms = duration_cast<milliseconds>(ts);
    return ms.count() > 0 ? static_cast<uint64_t>(ms.count()) : 0;
}

media::GlobalSystemMediaTransportControlsSessionManager session_manager() {
    return media::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
}

std::string display_name(std::string_view app_id) {
    if (app_id.empty()) return "Unknown media session";

    std::string out{app_id};
    const auto bang = out.rfind('!');
    if (bang != std::string::npos && bang + 1u < out.size()) out = out.substr(bang + 1u);

    const auto dot = out.rfind('.');
    if (dot != std::string::npos && dot + 1u < out.size()) {
        const auto tail = out.substr(dot + 1u);
        if (!tail.empty() && tail.size() <= 32u) out = tail;
    }

    std::replace(out.begin(), out.end(), '_', ' ');
    return out.empty() ? std::string{"Unknown media session"} : out;
}

std::optional<media::GlobalSystemMediaTransportControlsSession>
pick_session(media::GlobalSystemMediaTransportControlsSessionManager const& manager,
             std::string_view selected_id) {
    const auto sessions = manager.GetSessions();

    if (!selected_id.empty()) {
        // A configured selection must match exactly. Never fall back to another
        // app, or we'd read or control the wrong one while the chosen session
        // is momentarily absent (e.g. when it's paused).
        for (uint32_t i = 0; i < sessions.Size(); ++i) {
            auto session = sessions.GetAt(i);
            if (s(session.SourceAppUserModelId()) == selected_id) return session;
        }
        return std::nullopt;
    }

    if (const auto current = manager.GetCurrentSession()) return current;
    if (sessions.Size() > 0) return sessions.GetAt(0);
    return std::nullopt;
}

TrackInfo track_from_session(media::GlobalSystemMediaTransportControlsSession const& session,
                             uint64_t fallback_position_ms) {
    TrackInfo info;
    const auto props = session.TryGetMediaPropertiesAsync().get();

    info.title = s(props.Title());
    info.artist = s(props.Artist());
    if (info.artist.empty()) info.artist = s(props.AlbumArtist());
    info.album = s(props.AlbumTitle());

    try {
        const auto timeline = session.GetTimelineProperties();
        info.position_ms = ms_from_timespan(timeline.Position());
        const auto start = ms_from_timespan(timeline.StartTime());
        const auto end = ms_from_timespan(timeline.EndTime());
        if (end > start) info.duration_ms = end - start;
    } catch (...) {
        info.position_ms = fallback_position_ms;
    }

    if (info.title.empty()) info.title = "External Audio";
    if (info.artist.empty()) info.artist = display_name(s(session.SourceAppUserModelId()));
    return info;
}

// Run a transport command against the selected (or current) session. Returns
// false when nothing matches or the call throws.
template <class Fn>
bool with_session(std::string_view selected_id, Fn&& fn) {
    try {
        RoApartment apartment;
        auto manager = session_manager();
        auto session = pick_session(manager, selected_id);
        return session ? fn(*session) : false;
    } catch (...) {
        return false;
    }
}

#endif

} // namespace

bool external_audio_media_sessions_available() noexcept {
#if FH6_EXTERNAL_AUDIO_HAS_CPPWINRT
    return true;
#else
    return false;
#endif
}

std::vector<ExternalAudioMediaSession>
enumerate_external_audio_media_sessions(std::string_view selected_id) {
    std::vector<ExternalAudioMediaSession> out;
#if FH6_EXTERNAL_AUDIO_HAS_CPPWINRT
    try {
        RoApartment apartment;
        auto manager = session_manager();
        const auto current = manager.GetCurrentSession();
        const auto current_id = current ? s(current.SourceAppUserModelId()) : std::string{};
        const auto sessions = manager.GetSessions();

        out.reserve(sessions.Size());
        for (uint32_t i = 0; i < sessions.Size(); ++i) {
            auto session = sessions.GetAt(i);
            ExternalAudioMediaSession item;
            item.id = s(session.SourceAppUserModelId());
            item.name = display_name(item.id);
            item.is_current = !current_id.empty() && item.id == current_id;
            item.is_selected = !selected_id.empty() && item.id == selected_id;

            if (!item.id.empty()) out.push_back(std::move(item));
        }

        std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
            if (a.is_current != b.is_current) return a.is_current && !b.is_current;
            if (a.is_selected != b.is_selected) return a.is_selected && !b.is_selected;
            return a.name < b.name;
        });
    } catch (...) {}
#else
    (void)selected_id;
#endif
    return out;
}

std::optional<TrackInfo> external_audio_media_session_track(std::string_view selected_id,
                                                            uint64_t fallback_position_ms) {
#if FH6_EXTERNAL_AUDIO_HAS_CPPWINRT
    try {
        RoApartment apartment;
        auto manager = session_manager();
        auto session = pick_session(manager, selected_id);
        if (!session) return std::nullopt;
        return track_from_session(*session, fallback_position_ms);
    } catch (...) {
        return std::nullopt;
    }
#else
    (void)selected_id;
    (void)fallback_position_ms;
    return std::nullopt;
#endif
}

bool external_audio_media_session_next(std::string_view selected_id) {
#if FH6_EXTERNAL_AUDIO_HAS_CPPWINRT
    return with_session(selected_id, [](auto& s) { return s.TrySkipNextAsync().get(); });
#else
    (void)selected_id;
    return false;
#endif
}

bool external_audio_media_session_previous(std::string_view selected_id) {
#if FH6_EXTERNAL_AUDIO_HAS_CPPWINRT
    return with_session(selected_id, [](auto& s) { return s.TrySkipPreviousAsync().get(); });
#else
    (void)selected_id;
    return false;
#endif
}

bool external_audio_media_session_pause(std::string_view selected_id) {
#if FH6_EXTERNAL_AUDIO_HAS_CPPWINRT
    return with_session(selected_id, [](auto& s) { return s.TryPauseAsync().get(); });
#else
    (void)selected_id;
    return false;
#endif
}

bool external_audio_media_session_play(std::string_view selected_id) {
#if FH6_EXTERNAL_AUDIO_HAS_CPPWINRT
    return with_session(selected_id, [](auto& s) { return s.TryPlayAsync().get(); });
#else
    (void)selected_id;
    return false;
#endif
}

} // namespace fh6::sources
