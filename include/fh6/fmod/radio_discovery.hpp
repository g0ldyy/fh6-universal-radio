#pragma once

#include "fh6/fmod/pe_image.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace fh6::fmod_bridge {

struct RadioInstance {
    std::byte* refcount_obj;       // _Ref_count_obj2<RadioStreamFmod> on the heap
    std::byte* radio_stream;       // inline RadioStreamFmod (= refcount_obj + 16)
    std::byte* sample_props_body;  // resolved via +0x48 -> +0x18; holds the
                                   // std::string slots the game renders on the
                                   // radio HUD (SoundName/DisplayName/Artist).
    std::string sound_name;        // sample_props_body + 0x10
    std::uint64_t title_capacity = 0;
    std::uint64_t artist_capacity = 0;
};

struct DiscoveryResult {
    std::byte* vtable = nullptr;
    std::vector<RadioInstance> instances; // chain-validated instances
};

// Resolve FMOD's SystemI* from a RadioStreamFmod (+0x08 -> +0xC0, validated).
struct PEImage;
void* resolve_fmod_system(const PEImage& img, std::byte* radio_stream) noexcept;

// Locate every live, chain-validated RadioStreamFmod:
//   1) find the _Ref_count_obj2<RadioStreamFmod> RTTI typedesc,
//   2) follow it to the Complete Object Locator (COL),
//   3) scan for vtable candidates (COL sits at vtable[-1]),
//   4) walk the heap for refcount objects pointing at the candidate vtable,
//   5) drop hits whose SoundName chain doesn't resolve.
// Empty result means the radio system isn't ready yet; retry later.
DiscoveryResult discover_radio_instances(const PEImage& img) noexcept;
void reset_radio_discovery_cache() noexcept;

} // namespace fh6::fmod_bridge
