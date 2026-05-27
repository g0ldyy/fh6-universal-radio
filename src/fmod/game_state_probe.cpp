#include "fh6/fmod/game_state_probe.hpp"
#include "fh6/fmod/sig_scanner.hpp"
#include "fh6/log.hpp"
#include "fh6/safe_mem.hpp"

#include <cstring>

namespace fh6::fmod_bridge {

namespace {

constexpr const char* kProloguePattern =
    "48 89 5C 24 08 48 89 54 24 10 57 48 83 EC 40 48 8B FA "
    "48 8B 1D ?? ?? ?? ?? "
    "48 85 DB 74 16 48 8D 4C 24 20 E8 ?? ?? ?? ?? 48 8B D0 48 8B CB";

constexpr std::size_t kMovRipOffset  = 18;
constexpr std::size_t kDispOffset    = kMovRipOffset + 3;
constexpr std::size_t kInsnEndOffset = kMovRipOffset + 7;

// Offsets within *radio_state.
constexpr std::ptrdiff_t kRaceRunningA   = 0x68;
constexpr std::ptrdiff_t kRaceRunningB   = 0x69;
constexpr std::ptrdiff_t kRaceRestartDw  = 0x80;

constexpr std::ptrdiff_t kStationChain1Off = 0x40;
constexpr std::ptrdiff_t kStationChain2Off = 0x50;
constexpr std::ptrdiff_t kStationNameOff   = 0x200;
constexpr const char* kTargetStation1 = "Streamer Mode";
constexpr const char* kTargetStation2 = "Universal Radio";

} // namespace

GameStateProbe::GameStateProbe(const PEImage& img) noexcept {
    std::byte* match = find_by_pattern(img, kProloguePattern);
    if (!match) {
        log::warn("[gstate] radio_state_singleton pattern not found -- "
                  "raceStartPlayback will stay inert");
        return;
    }
    int32_t disp = 0;
    std::memcpy(&disp, match + kDispOffset, 4);
    const auto* slot = reinterpret_cast<const void* const*>(match + kInsnEndOffset + disp);
    if (slot < reinterpret_cast<const void* const*>(img.base) ||
        slot >= reinterpret_cast<const void* const*>(img.base + img.size)) {
        log::warn("[gstate] decoded singleton slot 0x{:X} outside FH6 image",
                  reinterpret_cast<uintptr_t>(slot));
        return;
    }
    singleton_slot_ = slot;
    log::info("[gstate] radio_state slot @ RVA 0x{:X}",
              static_cast<uint32_t>(reinterpret_cast<const std::byte*>(slot) - img.base));
}

GameStateProbe::Snapshot GameStateProbe::read() const noexcept {
    Snapshot out{};
    if (!singleton_slot_) return out;

    // Re-deref every tick: FH6 reallocates the radio_state across world
    // loads, so the slot's value isn't stable.
    const std::byte* radio_state = nullptr;
    if (!safe_read(singleton_slot_, radio_state) || !radio_state) return out;

    uint8_t a = 0, b = 0;
    int32_t restart = 0;
    if (safe_read(radio_state + kRaceRunningA, a) &&
        safe_read(radio_state + kRaceRunningB, b))
        out.race_active = a != 0 && b != 0;
    if (safe_read(radio_state + kRaceRestartDw, restart))
        out.race_restart = restart == -1;

    // Walk to the station-name std::string. Every link can be re-allocated
    // by FH6 (world load, scene swap) so we deref through each step.
    const std::byte* chain1 = nullptr;
    const std::byte* chain2 = nullptr;
    if (safe_read(radio_state + kStationChain1Off, chain1) && chain1 &&
        safe_read(chain1 + kStationChain2Off, chain2) && chain2) {
        if (auto name = safe_read_msvc_string(chain2 + kStationNameOff))
            out.on_target_station = (*name == kTargetStation1) || (*name == kTargetStation2);
    }
    return out;
}

} // namespace fh6::fmod_bridge
