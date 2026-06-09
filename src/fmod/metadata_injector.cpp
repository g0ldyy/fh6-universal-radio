#include "fh6/fmod/metadata_injector.hpp"
#include "fh6/log.hpp"
#include "fh6/safe_mem.hpp"

#include <windows.h>
#include <cstring>
#include <algorithm>

namespace fh6::fmod_bridge {

namespace {

constexpr std::ptrdiff_t kTitleOffset  = 0x30;
constexpr std::ptrdiff_t kArtistOffset = 0x50;

// MSVC std::string (x64, 32 bytes):
//   [0..15]  SSO buffer (16 bytes) OR heap pointer at +0
//   [16..23] size
//   [24..31] capacity (15 means SSO, anything > 15 means heap)
struct StringHeader {
    std::byte sso_buf[16];
    std::uint64_t size;
    std::uint64_t cap;
};
static_assert(sizeof(StringHeader) == 32);

constexpr std::uint64_t kSsoCap = 15;
constexpr std::uint64_t kMaxInjectedLen = 384;

bool write_string_slot(std::byte* target, std::string_view src) noexcept {
    if (!target) return false;

    // Snapshot the existing header. SEH-wrapped: a stale page would
    // otherwise take the game's audio thread down with us.
    StringHeader hdr{};
    if (!safe_read(target, hdr)) return false;
    if (hdr.cap < kSsoCap) return false; // implausible -- not an std::string
    if (hdr.size > hdr.cap) return false;

    const auto clipped = src.substr(0, static_cast<std::size_t>(std::min<std::uint64_t>(
                                      static_cast<std::uint64_t>(src.size()), kMaxInjectedLen)));
    const std::uint64_t new_size = static_cast<std::uint64_t>(clipped.size());

    auto inplace_overwrite_heap = [&]() -> bool {
        std::byte* heap = nullptr;
        std::memcpy(&heap, hdr.sso_buf, sizeof(heap));
        if (!heap) return false;
        if (!seh_call([&] {
                std::memcpy(heap, clipped.data(), clipped.size());
                heap[clipped.size()] = std::byte{0};
                std::memcpy(target + 16, &new_size, sizeof(new_size));
            }))
            return false;
        return true;
    };

    auto inplace_overwrite_sso = [&]() -> bool {
        if (!seh_call([&] {
                std::memcpy(target, clipped.data(), clipped.size());
                target[clipped.size()] = std::byte{0};
                std::memcpy(target + 16, &new_size, sizeof(new_size));
                // Cap stays at 15 (SSO) -- we don't touch it.
            }))
            return false;
        return true;
    };

    auto allocate_and_swap = [&]() -> bool {
        // FH6's HUD strings are often still in MSVC's 15-byte SSO storage.
        // To show full custom metadata, give the slot a process-local buffer
        // with headroom and leave the old game-owned storage untouched.
        const std::uint64_t alloc_cap = std::max<std::uint64_t>(
            kSsoCap + 1, (new_size + 32) & ~static_cast<std::uint64_t>(15));
        const SIZE_T bytes = static_cast<SIZE_T>(alloc_cap + 1);
        auto* fresh = static_cast<std::byte*>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes));
        if (!fresh) {
            log::warn("[meta] HeapAlloc failed for {} bytes", bytes);
            return false;
        }
        std::memcpy(fresh, clipped.data(), clipped.size());
        fresh[clipped.size()] = std::byte{0};

        if (!seh_call([&] {
                std::memcpy(target, &fresh, sizeof(fresh));
                std::memset(target + sizeof(fresh), 0, 16 - sizeof(fresh));
                std::memcpy(target + 16, &new_size, sizeof(new_size));
                std::memcpy(target + 24, &alloc_cap, sizeof(alloc_cap));
            })) {
            HeapFree(GetProcessHeap(), 0, fresh);
            return false;
        }
        log::info("[meta] expanded HUD string storage to {} bytes", alloc_cap);
        return true;
    };

    const bool is_heap = hdr.cap > kSsoCap;
    if (is_heap && new_size <= hdr.cap) return inplace_overwrite_heap();
    if (!is_heap && new_size <= kSsoCap) return inplace_overwrite_sso();
    return allocate_and_swap();
}

} // namespace

void MetadataInjector::set_target(std::byte* sample_props_body) noexcept {
    if (body_ == sample_props_body) return;
    body_ = sample_props_body;
    reset_cache();
    log::info("[meta] target set to SampleProperties body @0x{:X}",
              reinterpret_cast<uintptr_t>(body_));
}

void MetadataInjector::reset_cache() noexcept {
    last_title_.clear();
    last_artist_.clear();
}

bool MetadataInjector::update(std::string_view title, std::string_view artist) noexcept {
    if (!body_) return false;
    if (title == last_title_ && artist == last_artist_) return true;

    bool ok = true;
    if (title != last_title_) {
        if (title.size() > kMaxInjectedLen)
            log::warn("[meta] title clipped from {} to {} bytes", title.size(), kMaxInjectedLen);
        if (write_string_slot(body_ + kTitleOffset, title)) {
            last_title_.assign(title);
        } else {
            log::warn("[meta] title write failed (len={})", title.size());
            ok = false;
        }
    }
    if (artist != last_artist_) {
        if (artist.size() > kMaxInjectedLen)
            log::warn("[meta] artist clipped from {} to {} bytes", artist.size(), kMaxInjectedLen);
        if (write_string_slot(body_ + kArtistOffset, artist)) {
            last_artist_.assign(artist);
        } else {
            log::warn("[meta] artist write failed (len={})", artist.size());
            ok = false;
        }
    }
    if (ok) {
        log::info(R"([meta] wrote "{}" / "{}")", title, artist);
    }
    return ok;
}

} // namespace fh6::fmod_bridge
