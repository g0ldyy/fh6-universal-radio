#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace fh6::fmod_bridge {

// Overwrites the std::string slots the game reads to render the radio HUD,
// so the active source's title/artist show up in-game instead of the
// placeholder.
//
// The target is the SampleProperties body resolved during discovery (see
// RadioInstance::sample_props_body). Layout:
//   +0x10  SoundName    (left alone -- the game keys other things off it)
//   +0x30  DisplayName  (rendered as the song title)
//   +0x50  Artist       (rendered as the artist line)
//
// MSVC std::string is 32 bytes: 16-byte SSO buffer or heap pointer, then
// size and capacity. When the new content fits, we overwrite in place;
// otherwise we VirtualAlloc a fresh buffer and switch the slot to it.
// VirtualAlloc (not free()) is deliberate: we don't own the original
// allocator and freeing through the wrong one would corrupt the heap. The
// leak is one buffer per outgrown overwrite.
class MetadataInjector {
public:
    void set_target(std::byte* sample_props_body) noexcept;

    // Drops the cached strings so the next call rewrites unconditionally.
    void reset_cache() noexcept;

    // Idempotent: writes only when title/artist differ from the last
    // successful write. Returns true on a successful write (or no-op).
    bool update(std::string_view title, std::string_view artist) noexcept;

private:
    std::byte* body_ = nullptr;
    std::string last_title_;
    std::string last_artist_;
};

} // namespace fh6::fmod_bridge
