#include "fh6/fmod/radio_discovery.hpp"
#include "fh6/log.hpp"
#include "fh6/safe_mem.hpp"

#include <windows.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string_view>

namespace fh6::fmod_bridge {

namespace {

// MSVC RTTI typedesc = vfptr(8) + spare(8) + decorated_name. For
// std::_Ref_count_obj2<T> the decorated name is
// `.?AV?$_Ref_count_obj2@V<T>@@@std@@`; locate "RadioStreamFmod" preceded
// by "_Ref_count_obj2" and the `.?AV` RTTI class prefix.
const std::byte* find_typedesc_in(const std::byte* s, const std::byte* e) noexcept {
    constexpr std::string_view kLeaf  = "RadioStreamFmod";
    constexpr std::string_view kOuter = "_Ref_count_obj2";
    if (!s || !e || static_cast<std::size_t>(e - s) < kLeaf.size() + 16) return nullptr;

    for (const std::byte* p = s; p + kLeaf.size() < e; ++p) {
        if (std::memcmp(p, kLeaf.data(), kLeaf.size()) != 0) continue;

        const std::byte* lo = (p - s > 128) ? p - 128 : s;
        bool has_outer      = false;
        for (const std::byte* q = lo; q + kOuter.size() <= p; ++q) {
            if (std::memcmp(q, kOuter.data(), kOuter.size()) == 0) {
                has_outer = true;
                break;
            }
        }
        if (!has_outer) continue;

        for (const std::byte* q = p; q - s >= 4; --q) {
            if (q[-2] == std::byte{'.'} && q[-1] == std::byte{'?'} && q[0] == std::byte{'A'} &&
                q[1] == std::byte{'V'}) {
                return (q - 2 - s >= 16) ? q - 2 - 16 : nullptr;
            }
        }
    }
    return nullptr;
}

// FH6 puts the typedesc in a custom RTTI segment outside .rdata, so we
// have to scan every readable section.
const std::byte* find_typedesc(const PEImage& img) noexcept {
    for (const auto& sec : img.sections) {
        if (!sec.readable()) continue;
        if (const auto* td = find_typedesc_in(sec.start, sec.end)) return td;
    }
    return nullptr;
}

// COL has signature 0x01 at +0 and the typedesc RVA at +12.
const std::byte* find_col(const PEImage& img, uint32_t typedesc_rva) noexcept {
    for (const auto& sec : img.sections) {
        if (!sec.readable() || sec.end <= sec.start + 24) continue;
        for (const std::byte* p = sec.start; p + 16 <= sec.end; p += 4) {
            uint32_t sig, td;
            std::memcpy(&sig, p, 4);
            std::memcpy(&td, p + 12, 4);
            if (sig == 1 && td == typedesc_rva) return p;
        }
    }
    return nullptr;
}

std::vector<std::byte*> find_vtable_candidates(const std::byte* s, const std::byte* e,
                                               const std::byte* target) noexcept {
    std::vector<std::byte*> out;
    if (e <= s + 8) return out;
    for (const std::byte* p = s; p + 8 <= e; p += 8) {
        const std::byte* v;
        std::memcpy(&v, p, 8);
        if (v == target) out.push_back(const_cast<std::byte*>(p) + 8);
    }
    return out;
}

// Heap scan: walk every committed private R/W region, look for 16-byte
// aligned slots whose first qword is `vtable` and whose use/weak refcounts
// (next 8 bytes) sit in (0, 0x80]. SEH-wrapped per region, because a page
// freed between VirtualQuery and the read would otherwise kill the process.
constexpr std::size_t kMaxHits = 64;

void scan_region(const PEImage& img, const std::byte* vtable, std::byte* p, std::byte* end,
                 std::vector<std::byte*>& out) noexcept {
    seh_call([&] {
        for (; p + 24 <= end && out.size() < kMaxHits; p += 16) {
            std::byte* slot_vtable;
            std::memcpy(&slot_vtable, p, 8);
            if (slot_vtable != vtable) continue;

            uint32_t use, weak;
            std::memcpy(&use, p + 8, 4);
            std::memcpy(&weak, p + 12, 4);
            if (!use || !weak || use > 0x80 || weak > 0x80) continue;

            std::byte* inner_vtable = nullptr;
            std::memcpy(&inner_vtable, p + 16, 8);
            if (inner_vtable < img.base || inner_vtable >= img.base + img.size) continue;

            out.push_back(p);
        }
    });
}

std::vector<std::byte*> scan_heap(const PEImage& img, const std::byte* vtable) noexcept {
    std::vector<std::byte*> out;
    MEMORY_BASIC_INFORMATION mbi{};
    std::byte* addr           = nullptr;
    const std::byte* prev_end = nullptr;

    while (VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi) && out.size() < kMaxHits) {
        auto* region          = reinterpret_cast<std::byte*>(mbi.BaseAddress);
        std::byte* region_end = region + mbi.RegionSize;

        const bool readable = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
                              (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_WRITECOPY ||
                               mbi.Protect == PAGE_READONLY) &&
                              (mbi.Protect & PAGE_GUARD) == 0;
        const bool overlaps_module = region >= img.base && region < img.base + img.size;

        if (readable && mbi.RegionSize <= 0x4000000 && !overlaps_module) {
            auto* p   = reinterpret_cast<std::byte*>((uintptr_t)(region + 15) & ~uintptr_t{15});
            auto* end = reinterpret_cast<std::byte*>((uintptr_t)region_end & ~uintptr_t{7});
            scan_region(img, vtable, p, end, out);
        }
        if (region_end <= prev_end) break;
        prev_end = region_end;
        addr     = region_end;
    }
    return out;
}

// SoundName chain on a RadioStreamFmod (SEH-wrapped for safety):
//   +0x48 -> SampleProperties wrapper
//     +0x18 -> SampleProperties body
//       +0x10 -> std::string SoundName
//       +0x30 -> std::string DisplayName  (rendered as the song title)
//       +0x50 -> std::string Artist
// `step` reports which link broke first (0/1/2) so the periodic log can
// say which side of the chain is stuck. `out_body` receives the
// SampleProperties body pointer so the caller can later overwrite the
// DisplayName/Artist slots.
std::string read_sound_name(std::byte* radio_stream, int* step, std::byte** out_body) noexcept {
    std::string out;
    *step = 0;
    if (out_body) *out_body = nullptr;
    std::byte* a = nullptr;
    if (!safe_read(radio_stream + 0x48, a) || !a) return {};
    *step        = 1;
    std::byte* b = nullptr;
    if (!safe_read(a + 0x18, b) || !b) return {};
    *step = 2;
    if (out_body) *out_body = b;
    seh_call([&] {
        auto s = safe_read_msvc_string(b + 0x10);
        if (s) out = std::move(*s);
    });
    return out;
}

// Cache typedesc/COL/vtable + the heap-candidate list once located; retries
// only re-walk the SoundName chain on the cached refcount addresses.
struct Cache {
    bool located              = false;
    const std::byte* typedesc = nullptr;
    const std::byte* col      = nullptr;
    std::byte* vtable         = nullptr;
    std::vector<std::byte*> candidates;
    int empty_streak = 0; // chain-invalid retries since last hit
};
std::mutex g_cache_mu;
Cache g_cache;

// If the cached candidates never resolve (Forza allocated a placeholder
// wrapper we latched onto, but the real RadioStreamFmod instances haven't
// been allocated yet), drop the cache after this many empty retries so the
// next call rescans the heap. Old logs show real wrappers take ~1.5 min to
// chain-validate, so the threshold is generous enough to avoid thrash.
constexpr int kRescanThreshold = 30;

} // namespace

void* resolve_fmod_system(const PEImage& img, std::byte* radio_stream) noexcept {
    if (!radio_stream) return nullptr;
    void* out = nullptr;
    seh_call([&] {
        // radio_stream +0x08 -> X (first field past vtable), X +0xC0 -> SystemI*.
        std::byte* x = nullptr;
        if (!safe_read(radio_stream + 0x08, x) || !x) return;
        std::byte* sys = nullptr;
        if (!safe_read(x + 0xC0, sys) || !sys) return;
        std::byte* vt = nullptr;
        if (!safe_read(sys, vt) || vt < img.base || vt >= img.base + img.size) return;
        out = sys;
    });
    return out;
}

DiscoveryResult discover_radio_instances(const PEImage& img) noexcept {
    DiscoveryResult result;
    if (!img.valid()) return result;

    Cache local;
    {
        std::scoped_lock lk{g_cache_mu};
        local = g_cache;
    }

    // First call only: typedesc + COL + vtable + heap scan.
    if (!local.located) {
        const std::byte* td = find_typedesc(img);
        if (!td) {
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true, std::memory_order_acq_rel)) {
                log::warn("[discovery] _Ref_count_obj2<RadioStreamFmod> typedesc not "
                          "found in any readable section. Check the media overlay.");
            }
            return result;
        }
        const auto td_rva = static_cast<uint32_t>(td - img.base);
        log::info("[discovery] typedesc RadioStreamFmod @ RVA=0x{:X}", td_rva);

        const std::byte* col = find_col(img, td_rva);
        if (!col) {
            log::warn("[discovery] typedesc found but no matching COL -- RTTI layout differs");
            return result;
        }
        log::info("[discovery] COL @ RVA=0x{:X}", static_cast<uint32_t>(col - img.base));

        // Try .rdata first (where vtables usually live), then the full image.
        std::byte* found_vt = nullptr;
        std::vector<std::byte*> found_candidates;
        for (int pass = 0; pass < 2 && found_candidates.empty(); ++pass) {
            const std::byte* s = pass == 0 ? img.rdata : img.base;
            const std::byte* e = pass == 0 ? img.rdata_end : img.base + img.size;
            for (auto* vt : find_vtable_candidates(s, e, col)) {
                std::byte* first_method;
                std::memcpy(&first_method, vt, 8);
                if (first_method < img.base || first_method >= img.base + img.size) continue;

                auto hits = scan_heap(img, vt);
                if (!hits.empty()) {
                    found_vt         = vt;
                    found_candidates = std::move(hits);
                    break;
                }
            }
        }
        if (found_candidates.empty()) {
            // Game hasn't allocated the wrappers yet; don't cache, retry later.
            log::info("[discovery] no heap candidates yet -- waiting for the radio system");
            return result;
        }
        log::info("[discovery] cached {} heap candidate(s) on vtable @ 0x{:X}",
                  found_candidates.size(), static_cast<uint32_t>(found_vt - img.base));

        local.located    = true;
        local.typedesc   = td;
        local.col        = col;
        local.vtable     = found_vt;
        local.candidates = std::move(found_candidates);
        {
            std::scoped_lock lk{g_cache_mu};
            g_cache = local;
        }
    }

    // Retry path: just re-validate the SoundName chain on cached candidates.
    int step_histogram[3] = {0, 0, 0};
    for (auto* refcount : local.candidates) {
        std::byte* radio_stream = refcount + 16;
        std::byte* body         = nullptr;
        int step                = 0;
        auto name               = read_sound_name(radio_stream, &step, &body);
        if (name.empty()) {
            ++step_histogram[step];
            continue;
        }
        result.vtable = local.vtable;
        result.instances.push_back({refcount, radio_stream, body, std::move(name)});
    }

    if (result.instances.empty()) {
        const bool invalidate = ++local.empty_streak >= kRescanThreshold;
        {
            std::scoped_lock lk{g_cache_mu};
            if (invalidate) {
                g_cache = Cache{}; // force a full heap rescan next call
            } else {
                g_cache.empty_streak = local.empty_streak;
            }
        }
        if (invalidate) {
            log::info("[discovery] cached candidates stayed chain-invalid for {} retries; "
                      "dropping cache to rescan the heap",
                      kRescanThreshold);
        } else {
            static std::atomic<int> tick{0};
            const int n = tick.fetch_add(1, std::memory_order_acq_rel);
            if (n % 6 == 0) { // ~30s at the 5s retry rate
                log::info("[discovery] {} heap candidates, none chain-valid yet "
                          "(chain breaks: +0x48={} +0x18={} string-empty={}). "
                          "Load into the game and cycle through radio stations.",
                          local.candidates.size(), step_histogram[0], step_histogram[1],
                          step_histogram[2]);
            }
        }
        return result;
    }

    // Only log the "found" list on the first hit after a miss streak (initial
    // discovery, or a recovery from a stale-cache rescan). Periodic callers
    // like the staleness watchdog would otherwise spam the log every second.
    const bool first_hit_after_misses = local.empty_streak != 0;
    if (first_hit_after_misses) {
        std::scoped_lock lk{g_cache_mu};
        g_cache.empty_streak = 0;
    }
    static std::atomic<bool> ever_logged{false};
    if (first_hit_after_misses || !ever_logged.exchange(true, std::memory_order_acq_rel)) {
        log::info("[discovery] found {} chain-valid RadioStreamFmod instance(s):",
                  result.instances.size());
        for (auto& i : result.instances) {
            log::info("[discovery]   @0x{:X}  SoundName=\"{}\"",
                      reinterpret_cast<uintptr_t>(i.radio_stream), i.sound_name);
        }
    }
    return result;
}

} // namespace fh6::fmod_bridge
