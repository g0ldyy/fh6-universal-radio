#include "fh6/fmod/radio_target_policy.hpp"
#include "fh6/fmod/dsp_retarget_policy.hpp"

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error{message};
}

} // namespace

int main() {
    try {
        using fh6::fmod_bridge::RetargetAction;
        using fh6::fmod_bridge::decide_retarget;
        using fh6::fmod_bridge::is_stale_recovery_false_alarm;
        using fh6::fmod_bridge::select_target_radio_instance;
        using fh6::fmod_bridge::should_skip_recovery_candidate;

        auto replace_stereo = decide_retarget(0x1000u, true, 2u, 0x2000u);
        require(replace_stereo.action == RetargetAction::install_slot,
                "stereo current DSP handle should follow a different live radio handle");
        require(replace_stereo.handle == 0x2000u,
                "stereo replacement should target the live slot handle");

        auto replace_mono = decide_retarget(0x1000u, true, 1u, 0x2000u);
        require(replace_mono.action == RetargetAction::install_slot,
                "mono current output should allow retargeting to a different live handle");
        require(replace_mono.handle == 0x2000u,
                "mono replacement should target the live slot handle");

        auto keep_mono_without_alternate = decide_retarget(0x1000u, true, 1u, 0x1000u);
        require(keep_mono_without_alternate.action == RetargetAction::keep_current,
                "mono current output should stay attached when no alternate handle is available");

        auto install_first = decide_retarget(0u, false, 0u, 0x2000u);
        require(install_first.action == RetargetAction::install_slot,
                "missing current handle should install a live slot handle");
        require(install_first.handle == 0x2000u, "install should target the live slot handle");

        auto release_dead = decide_retarget(0x1000u, false, 0u, 0u);
        require(release_dead.action == RetargetAction::release_current,
                "dead current handle with no replacement should release the DSP");
        require(release_dead.handle == 0u, "release should not carry a target handle");

        auto no_work = decide_retarget(0u, false, 0u, 0u);
        require(no_work.action == RetargetAction::none, "no handles should require no work");

        require(!is_stale_recovery_false_alarm(true, 2u),
                "a live stereo handle must not suppress recovery after callbacks stop");
        require(!is_stale_recovery_false_alarm(true, 1u),
                "a live mono handle must not suppress stale recovery after callbacks stop");
        require(!is_stale_recovery_false_alarm(false, 2u),
                "a dead handle must not suppress stale recovery");

        require(should_skip_recovery_candidate(0x3000u, 0x3000u, true),
                "live stale recovery should skip the same radio stream candidate");
        require(!should_skip_recovery_candidate(0x4000u, 0x3000u, true),
                "live stale recovery should accept a different radio stream candidate");
        require(!should_skip_recovery_candidate(0x3000u, 0x3000u, false),
                "dead-handle recovery may reuse the same radio stream candidate");

        fh6::fmod_bridge::DiscoveryResult disc;
        auto target_stream = reinterpret_cast<std::byte*>(0x1000);
        auto other_stream  = reinterpret_cast<std::byte*>(0x2000);
        disc.instances.push_back({nullptr, other_stream, nullptr, "HZ6_R5_Urbandawn_Superheim"});
        disc.instances.push_back(
            {nullptr, target_stream, nullptr, "HZ6_R9_PeterBroderick_EyesClosedandTraveling"});

        auto live          = [](std::byte*) { return true; };
        const auto* target = select_target_radio_instance(
            disc, "HZ6_R9_PeterBroderick_EyesClosedandTraveling", /*allow_fallback=*/true,
            /*require_live=*/true, nullptr, live);
        require(target && target->radio_stream == target_stream,
                "target selection should prefer the configured overlay carrier");

        fh6::fmod_bridge::DiscoveryResult original_only;
        original_only.instances.push_back(
            {nullptr, other_stream, nullptr, "HZ6_R5_Urbandawn_Superheim"});
        const auto* missing = select_target_radio_instance(
            original_only, "HZ6_R9_PeterBroderick_EyesClosedandTraveling", /*allow_fallback=*/false,
            /*require_live=*/false, nullptr, live);
        require(missing == nullptr,
                "target selection must not fall back to original FH6 radio samples");

        const auto* fallback = select_target_radio_instance(
            original_only, "HZ6_R9_PeterBroderick_EyesClosedandTraveling", /*allow_fallback=*/true,
            /*require_live=*/false, nullptr, live);
        require(fallback && fallback->radio_stream == other_stream,
                "streamer-mode selection should allow the active station sample as fallback");

        return 0;
    } catch (const std::runtime_error& e) {
        std::cerr << e.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "unknown dsp_retarget_policy_tests failure\n";
        return 1;
    }
}
