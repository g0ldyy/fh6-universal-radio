#pragma once

#include <cstdint>

namespace fh6::fmod_bridge {

enum class RetargetAction { none, keep_current, release_current, install_slot };

struct RetargetDecision {
    RetargetAction action = RetargetAction::none;
    uint32_t handle       = 0;
};

inline RetargetDecision decide_retarget(uint32_t current_handle, bool current_alive,
                                        uint32_t current_output_channels,
                                        uint32_t slot_handle) noexcept {
    static_cast<void>(current_output_channels);
    if (current_handle != 0 && current_alive && slot_handle == current_handle) {
        return {RetargetAction::keep_current, current_handle};
    }
    if (slot_handle != 0) {
        return {RetargetAction::install_slot, slot_handle};
    }
    if (current_handle != 0 && current_alive) {
        return {RetargetAction::keep_current, current_handle};
    }
    if (current_handle != 0) {
        return {RetargetAction::release_current, 0};
    }
    return {};
}

inline bool is_stale_recovery_false_alarm(bool current_alive,
                                          uint32_t current_output_channels) noexcept {
    static_cast<void>(current_alive);
    static_cast<void>(current_output_channels);
    return false;
}

inline bool should_skip_recovery_candidate(uintptr_t candidate_stream, uintptr_t current_stream,
                                           bool current_alive) noexcept {
    return current_alive && current_stream != 0 && candidate_stream == current_stream;
}

} // namespace fh6::fmod_bridge
