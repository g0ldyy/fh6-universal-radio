#pragma once

#include "fh6/fmod/dsp_retarget_policy.hpp"
#include "fh6/fmod/radio_discovery.hpp"

#include <cstdint>
#include <string_view>

namespace fh6::fmod_bridge {

template <typename IsLive>
const RadioInstance*
select_target_radio_instance(const DiscoveryResult& disc, std::string_view target_sound_name,
                             bool allow_fallback, bool require_live, std::byte* avoid_radio_stream,
                             IsLive&& is_live) noexcept {
    const RadioInstance* fallback = nullptr;
    for (const auto& i : disc.instances) {
        if (require_live && !is_live(i.radio_stream)) continue;
        if (should_skip_recovery_candidate(reinterpret_cast<uintptr_t>(i.radio_stream),
                                           reinterpret_cast<uintptr_t>(avoid_radio_stream),
                                           avoid_radio_stream != nullptr)) {
            continue;
        }
        if (i.sound_name == target_sound_name) return &i;
        if (allow_fallback && !fallback) fallback = &i;
    }
    return fallback;
}

} // namespace fh6::fmod_bridge
