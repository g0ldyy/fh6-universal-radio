#include "fh6/sources/local_file_source.hpp"
#include "fh6/log.hpp"
#include "fh6/subprocess.hpp"

#include <windows.h>
#include <cstdio>
#include <limits>

// miniaudio decodes mp3/flac/wav/ogg to S16LE/48k/stereo; everything else
// falls through to the ffmpeg fallback below.
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DEVICE_IO
#define MA_NO_GENERATION
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244) // narrowing inside miniaudio's header
#endif
#include <miniaudio.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <random>
#include <set>
#include <unordered_map>

namespace fh6::sources {

namespace {

constexpr std::uint32_t kSampleRate  = 48000;
constexpr std::size_t kFrameBytes    = 4; // s16 * 2ch
constexpr std::uint64_t kBytesPerSec = std::uint64_t{kSampleRate} * kFrameBytes;

using subprocess::capture_output;
using subprocess::create_kill_on_close_job;
using subprocess::describe_launch_failure;
using subprocess::open_nul;
using subprocess::open_stderr_log;
using subprocess::quote;
using subprocess::spawn_in_job;

bool extension_matches(const std::filesystem::path& p, const std::vector<std::string>& exts) {
    if (!p.has_extension()) return false;
    auto e = p.extension().string();
    if (!e.empty() && e.front() == '.') e.erase(0, 1);
    std::ranges::transform(e, e.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return std::ranges::find(exts, e) != exts.end();
}

/// Parse an .m3u / .m3u8 playlist: one entry per line, # comments,
/// extended info (#EXTINF) is ignored.
std::vector<std::filesystem::path> parse_m3u_playlist(const std::filesystem::path& file) {
    std::vector<std::filesystem::path> out;
    std::ifstream in(file);
    if (!in) return out;
    const auto dir = file.parent_path();
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::filesystem::path p = std::filesystem::path{line};
        if (p.is_relative()) p = dir / p;
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) out.push_back(std::move(p));
    }
    return out;
}

struct ProbedMetadata {
    std::uint64_t duration_ms = 0;
    std::string title, artist, album, album_artist;
    int track_no = 0, disc_no = 0;
};

// Leading integer of values like "5", "5/12", "05" -> 5; 0 when absent.
int parse_leading_int(std::string_view v) noexcept {
    int n    = 0;
    bool any = false;
    for (char c : v) {
        if (c < '0' || c > '9') break;
        any = true;
        n   = n * 10 + (c - '0');
    }
    return any ? n : 0;
}

// Cover format from magic bytes; "" for anything we wouldn't give an <img>.
std::string sniff_image_mime(const std::string& d) noexcept {
    auto u = [&](std::size_t i) { return static_cast<unsigned char>(d[i]); };
    if (d.size() >= 3 && u(0) == 0xFF && u(1) == 0xD8 && u(2) == 0xFF) return "image/jpeg";
    if (d.size() >= 8 && u(0) == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G')
        return "image/png";
    if (d.size() >= 6 && (d.starts_with("GIF87a") || d.starts_with("GIF89a"))) return "image/gif";
    if (d.size() >= 12 && d.starts_with("RIFF") && d.compare(8, 4, "WEBP") == 0)
        return "image/webp";
    return {};
}

// Copy the embedded cover out via one ffmpeg pass; empty when there's none.
// 8 MiB cap: covers are small and an unbounded read could balloon.
ArtworkImage extract_cover(const std::wstring& ff_bin, const std::filesystem::path& file,
                           worker::WorkerClient* worker) {
                           
    // create a unique temporary file path to prevent collisions
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path tmp = std::filesystem::temp_directory_path() / 
                                ("fh6_cover_" + std::to_string(now) + ".jpg");

    // tell FFmpeg to save the image directly to the temp file
    const std::wstring cmd =
        quote(ff_bin) + L" -hide_banner -nostdin -loglevel error -i " + quote(file.wstring()) +
        L" -an -c:v mjpeg -frames:v 1 -y " + quote(tmp.wstring());

    // output is written to disk
    if (worker && worker->alive()) {
        worker->run_capture(cmd);
    } else {
        capture_output(cmd, false, 0);
    }

    // read the pristine binary data back from the temp file
    std::string data;
    std::ifstream is{tmp, std::ios::binary | std::ios::ate};
    if (is) {
        auto size = is.tellg();
        is.seekg(0, std::ios::beg);
        if (size > 0 && size <= (8u << 20)) { // cap at 8MB
            data.resize(size);
            is.read(data.data(), size);
        }
    }
    
    // clean up
    std::error_code ec;
    std::filesystem::remove(tmp, ec);

    ArtworkImage out;
    out.mime = sniff_image_mime(data);
    if (!out.mime.empty()) out.bytes = std::move(data);
    return out;
}

bool ieq_str(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

bool decodes_as(UINT cp, std::string_view s) noexcept {
    return !s.empty() &&
           MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0) > 0;
}

std::string tag_to_utf8(std::string s) {
    if (s.empty() || decodes_as(CP_UTF8, s)) return s;
    if (!decodes_as(932, s)) return {};
    const int n = MultiByteToWideChar(932, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(932, 0, s.data(), (int)s.size(), w.data(), n);
    return subprocess::narrow(w);
}

// `ffmpeg -i <file>` with no output specified exits non-zero but first dumps
// the input header (Duration + container Metadata block) to stderr.
ProbedMetadata probe_metadata(const std::wstring& ff_bin, const std::filesystem::path& file,
                              worker::WorkerClient* worker) {
    ProbedMetadata out;
    const std::wstring cmd =
        quote(ff_bin) + L" -hide_banner -nostdin -i " + quote(file.wstring());

    const std::string err = (worker && worker->alive()) ? worker->run_capture(cmd, true)
                                                         : capture_output(cmd, true);

    if (auto pos = err.find("Duration:"); pos != std::string::npos) {
        pos += 9;
        while (pos < err.size() && err[pos] == ' ') ++pos;
        int h = 0, m = 0;
        double s = 0.0;
        // NOLINTNEXTLINE(cert-err34-c): return value checked; ffmpeg duration text is bounded
        if (std::sscanf(err.c_str() + pos, "%d:%d:%lf", &h, &m, &s) == 3)
            out.duration_ms = static_cast<std::uint64_t>((h * 3600 + m * 60) * 1000.0 + s * 1000.0);
    }

    // First non-empty match wins so the container block (emitted before any
    // per-stream block) beats codec-internal "encoder" tags.
    for (std::size_t lstart = 0; lstart < err.size();) {
        auto nl   = err.find('\n', lstart);
        auto stop = (nl == std::string::npos) ? err.size() : nl;
        std::string_view line(err.data() + lstart, stop - lstart);
        lstart = (nl == std::string::npos) ? err.size() : nl + 1;

        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        auto colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        auto key = line.substr(0, colon);
        while (!key.empty() && key.back() == ' ') key.remove_suffix(1);
        auto val = line.substr(colon + 1);
        while (!val.empty() && val.front() == ' ') val.remove_prefix(1);
        if (val.empty()) continue;

        if (out.title.empty() && ieq_str(key, "title")) {
            out.title.assign(val);
        } else if (out.artist.empty() && ieq_str(key, "artist")) {
            out.artist.assign(val);
        } else if (out.album.empty() && ieq_str(key, "album")) {
            out.album.assign(val);
        } else if (out.album_artist.empty() &&
                   (ieq_str(key, "album_artist") || ieq_str(key, "ALBUM ARTIST") ||
                    ieq_str(key, "albumartist"))) {
            out.album_artist.assign(val);
        } else if (out.track_no == 0 && ieq_str(key, "track")) {
            out.track_no = parse_leading_int(val);
        } else if (out.disc_no == 0 && (ieq_str(key, "disc") || ieq_str(key, "discnumber"))) {
            out.disc_no = parse_leading_int(val);
        }
    }
    // Normalize every text tag to UTF-8 so the index and WebUI never see raw
    // legacy bytes; undecodable values drop to "" and fall back to the filename.
    for (std::string* s : {&out.title, &out.artist, &out.album, &out.album_artist})
        *s = tag_to_utf8(std::move(*s));
    return out;
}

// Case-insensitive, separator-aware prefix test using normalized generic
// strings, so "C:/Music/Live" is detected as inside "C:/music".
std::string norm_path_key(const std::filesystem::path& p) {
    auto s = p.lexically_normal().generic_string();
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}
bool path_within(const std::string& child, const std::string& parent) noexcept {
    if (parent.empty() || child.size() < parent.size()) return false;
    if (!child.starts_with(parent)) return false;
    return child.size() == parent.size() || child[parent.size()] == '/';
}

std::string path_utf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string{u8.begin(), u8.end()};
}

std::mt19937& thread_rng() {
    static thread_local std::mt19937 g{std::random_device{}()};
    return g;
}

} // namespace

struct LocalFileSource::Decoder {
    ma_decoder ma{};
    bool ma_open = false;
    bool ma_eof  = false;

    worker::WorkerClient* worker = nullptr;
    uint32_t pipeline_id         = 0;
    HANDLE ff_job              = nullptr;
    HANDLE ff_proc             = nullptr;
    HANDLE ff_pipe             = nullptr;
    std::uint64_t ff_bytes_out = 0;
    bool ff_eof                = false;

    TrackInfo info{};
    ArtworkImage art{};
    // Per-decoder so a prefetched track promotes with its own ReplayGain
    // multiplier instead of inheriting the current track's.
    float loudness_coef    = 1.0f;
    std::size_t for_cursor = 0;

    bool any_open() const noexcept { return ma_open || ff_pipe != nullptr; }

    ~Decoder() {
        if (ma_open)  ma_decoder_uninit(&ma);
        if (ff_pipe) { CloseHandle(ff_pipe); ff_pipe = nullptr; }
        if (worker && pipeline_id) worker->kill_pipeline(pipeline_id);

        subprocess::reap(ff_proc); // direct-mode child (no-op in worker mode)
        // KILL_ON_JOB_CLOSE on the job reaps ffmpeg if it's still resident.
        if (ff_job) CloseHandle(ff_job);
    }
};

LocalFileSource::LocalFileSource(LocalFilesConfig cfg, std::filesystem::path ffmpeg_path,
                                 std::filesystem::path index_path, worker::WorkerClient* worker)
    : cfg_{std::move(cfg)}, ffmpeg_path_{std::move(ffmpeg_path)},
      index_path_{std::move(index_path)}, worker_{worker} {}

LocalFileSource::~LocalFileSource() {
    stop_tag_thread();
    close_current();
}

void LocalFileSource::shutdown() noexcept { close_current(); }

bool LocalFileSource::initialize() {
    if (!cfg_.enabled) return false;
    // No roots yet is fine. auth_state stays needs_auth until the user adds a
    // folder from the dashboard, which calls set_config() to rescan.
    rebuild_playlist();
    log::info("[local] station '{}': {} tracks", active_station_name(), track_count());
    return true;
}

const LocalStation* LocalFileSource::active_station_locked() const noexcept {
    if (cfg_.stations.empty()) return nullptr;
    for (const auto& s : cfg_.stations)
        if (s.name == cfg_.active_station) return &s;
    return &cfg_.stations.front();
}

bool LocalFileSource::is_shuffle_locked() const noexcept {
    const LocalStation* st = active_station_locked();
    return st && st->order == "shuffle";
}

std::string LocalFileSource::repeat_mode_locked() const {
    const LocalStation* st = active_station_locked();
    return st ? st->repeat : std::string{"all"};
}

void LocalFileSource::shuffle_in_place_locked(std::vector<std::filesystem::path>& v) const {
    std::shuffle(v.begin(), v.end(), thread_rng());
}

void LocalFileSource::order_album_by_folder_locked() {
    std::unordered_map<std::string, std::vector<std::filesystem::path>> groups;
    std::vector<std::string> order;
    for (auto& p : playlist_) {
        auto key            = p.parent_path().string();
        auto [it, inserted] = groups.try_emplace(key);
        if (inserted) order.push_back(key);
        it->second.push_back(std::move(p));
    }
    for (auto& [_, v] : groups) std::ranges::sort(v);       // track order within the album
    std::shuffle(order.begin(), order.end(), thread_rng()); // album order
    playlist_.clear();
    for (auto& key : order)
        for (auto& p : groups[key]) playlist_.push_back(std::move(p));
}

void LocalFileSource::apply_order_locked(const LocalStation& st) {
    if (st.order == "name") {
        std::ranges::sort(playlist_);
    } else if (st.order == "folder") {
        std::ranges::sort(playlist_, [](const auto& a, const auto& b) {
            const auto pa = a.parent_path(), pb = b.parent_path();
            return pa != pb ? pa < pb : a.filename() < b.filename();
        });
    } else if (st.order == "album") {
        order_album_by_folder_locked(); // immediate; tag re-sort refines it later
    } else {
        shuffle_in_place_locked(playlist_); // "shuffle"
    }
}

void LocalFileSource::rebuild_playlist() {
    stop_tag_thread(); // join any in-flight tag re-sort before mutating state

    // Snapshot what we need, then scan off-lock: the directory walk and .m3u
    // parsing are heavy IO and pump() also takes mu_, so scanning under the lock
    // would stall the audio thread on large libraries. Mirrors index_worker's
    // snapshot -> work -> reacquire -> gen-check handover.
    LocalStation st;
    std::vector<std::string> formats;
    std::filesystem::path cur;
    std::uint64_t gen;
    {
        std::scoped_lock lk{mu_};
        discard_prefetch_locked(); // playlist contents/order are about to change
        gen                   = ++rebuild_gen_;
        const LocalStation* a = active_station_locked();
        if (!a || a->roots.empty()) {
            playlist_.clear();
            cursor_ = 0;
            dec_.reset(); // no station -> nothing to play
            return;
        }
        st      = *a;
        formats = cfg_.supported_formats;
        cur = (dec_ && cursor_ < playlist_.size()) ? playlist_[cursor_] : std::filesystem::path{};
    }

    std::vector<std::filesystem::path> built;
    std::set<std::filesystem::path> seen;

    std::vector<std::string> excluded;
    excluded.reserve(st.excluded.size());
    for (const auto& e : st.excluded) excluded.push_back(norm_path_key(e));
    auto is_excluded = [&](const std::filesystem::path& dir) {
        const auto key = norm_path_key(dir);
        for (const auto& ex : excluded)
            if (path_within(key, ex)) return true;
        return false;
    };
    auto add_file = [&](const std::filesystem::path& p) {
        const auto ext = p.extension().string();
        if (ieq_str(ext, ".m3u") || ieq_str(ext, ".m3u8")) {
            for (auto& entry : parse_m3u_playlist(p)) {
                if (extension_matches(entry, formats) && seen.insert(entry).second)
                    built.push_back(std::move(entry));
            }
        } else if (extension_matches(p, formats) && seen.insert(p).second) {
            built.push_back(p);
        }
    };

    for (const auto& root : st.roots) {
        std::error_code ec;
        if (root.empty() || !std::filesystem::exists(root, ec)) continue;
        if (st.recursive) {
            std::filesystem::recursive_directory_iterator it(
                root, std::filesystem::directory_options::skip_permission_denied, ec),
                end;
            for (; it != end && !ec; it.increment(ec)) {
                std::error_code fe;
                if (it->is_directory(fe)) {
                    if (is_excluded(it->path())) it.disable_recursion_pending();
                } else if (it->is_regular_file(fe)) {
                    add_file(it->path());
                }
            }
        } else {
            for (const auto& e : std::filesystem::directory_iterator(root, ec))
                if (e.is_regular_file(ec)) add_file(e.path());
        }
    }

    std::scoped_lock lk{mu_};
    if (rebuild_gen_.load(std::memory_order_acquire) != gen) return; // superseded mid-scan
    discard_prefetch_locked(); // pump() may have prefetched against the old queue
    playlist_ = std::move(built);
    apply_order_locked(st);
    // Keep the current track playing if the rebuilt queue still has it (an order
    // change within the same station); otherwise it's a different station, so
    // drop the decoder and let play() cut over to the new queue.
    cursor_ = 0;
    if (!cur.empty()) {
        auto it = std::ranges::find(playlist_, cur);
        if (it != playlist_.end()) {
            cursor_ = (std::size_t)(it - playlist_.begin());
        } else {
            dec_.reset();
        }
    }
    // Populate the metadata index in the background for every station so the
    // queue shows real titles and search matches them; reorder by tags only
    // when the station asks for album/tag grouping.
    start_index_locked(gen, st.order == "album" && st.grouping == "tags");
}

// ---- tag-grouping background index -----------------------------------------

void LocalFileSource::stop_tag_thread() {
    tag_cancel_.store(true, std::memory_order_release);
    if (tag_thread_.joinable()) tag_thread_.join();
    tag_cancel_.store(false, std::memory_order_release);
}

void LocalFileSource::start_index_locked(std::uint64_t gen, bool resort) {
    if (playlist_.empty()) return;
    auto paths      = playlist_;
    std::wstring ff = ffmpeg_path_.empty() ? L"ffmpeg" : ffmpeg_path_.wstring();
    tag_thread_ = std::thread(&LocalFileSource::index_worker, this, std::move(paths), std::move(ff),
                              gen, resort);
}

void LocalFileSource::load_index_if_needed() {
    std::scoped_lock il{index_mu_};
    if (index_loaded_) return;
    index_loaded_ = true;
    if (index_path_.empty()) return;
    std::ifstream in(index_path_, std::ios::binary);
    if (!in) return;
    try {
        nlohmann::json j;
        in >> j;
        for (const auto& [k, v] : j.items()) {
            TrackMeta m;
            m.mtime        = v.value("mtime", std::int64_t{0});
            m.album        = v.value("album", std::string{});
            m.album_artist = v.value("album_artist", std::string{});
            m.title        = v.value("title", std::string{});
            m.artist       = v.value("artist", std::string{});
            m.track_no     = v.value("track_no", 0);
            m.disc_no      = v.value("disc_no", 0);
            m.duration_ms  = v.value("duration_ms", std::uint64_t{0});
            index_[k]      = std::move(m);
        }
    } catch (...) {}
}

void LocalFileSource::save_index() {
    if (index_path_.empty()) return;
    nlohmann::json j = nlohmann::json::object();
    {
        std::scoped_lock il{index_mu_};
        for (auto& [k, m] : index_) {
            j[k] = nlohmann::json{{"mtime", m.mtime},
                                  {"album", m.album},
                                  {"album_artist", m.album_artist},
                                  {"title", m.title},
                                  {"artist", m.artist},
                                  {"track_no", m.track_no},
                                  {"disc_no", m.disc_no},
                                  {"duration_ms", m.duration_ms}};
        }
    }
    std::error_code ec;
    auto tmp  = index_path_;
    tmp      += ".tmp";
    {
        std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
        if (!os) return;
        os << j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    std::filesystem::rename(tmp, index_path_, ec);
    if (ec) {
        // Windows rename() won't clobber an existing target; fall back to
        // replace, and on any failure drop the temp so it doesn't accumulate.
        std::error_code rep;
        std::filesystem::remove(index_path_, rep);
        std::filesystem::rename(tmp, index_path_, rep);
        if (rep) {
            log::warn("[local] could not write metadata index {}: {}", index_path_.string(),
                      rep.message());
            std::filesystem::remove(tmp, rep);
        }
    }
}

std::vector<std::filesystem::path>
LocalFileSource::order_album_by_tags(const std::vector<std::filesystem::path>& in) {
    struct Item {
        std::filesystem::path path;
        int disc = 0, track = 0;
        std::string fname;
    };
    std::unordered_map<std::string, std::vector<Item>> groups;
    std::vector<std::string> order;
    {
        std::scoped_lock il{index_mu_};
        for (const auto& p : in) {
            std::string gkey;
            int disc_no = 0, track_no = 0;
            auto it = index_.find(path_utf8(p));
            if (it != index_.end() &&
                !(it->second.album.empty() && it->second.album_artist.empty())) {
                const auto& m = it->second;
                gkey     = (m.album_artist.empty() ? m.artist : m.album_artist) + '\x01' + m.album;
                disc_no  = m.disc_no;
                track_no = m.track_no;
            } else {
                gkey = std::string{'\x02'} + p.parent_path().string(); // fallback: by folder
            }
            auto [g, ins] = groups.try_emplace(gkey);
            if (ins) order.push_back(gkey);
            g->second.push_back(Item{p, disc_no, track_no, p.filename().string()});
        }
    }
    for (auto& [_, v] : groups) {
        std::ranges::sort(v, [](const Item& a, const Item& b) {
            if (a.disc != b.disc) return a.disc < b.disc;
            if (a.track != b.track) return a.track < b.track;
            return a.fname < b.fname;
        });
    }
    std::shuffle(order.begin(), order.end(), thread_rng());
    std::vector<std::filesystem::path> out;
    out.reserve(in.size());
    for (auto& k : order)
        for (auto& item : groups[k]) out.push_back(std::move(item.path));
    return out;
}

void LocalFileSource::index_worker(const std::vector<std::filesystem::path>& paths,
                                   const std::wstring& ff, std::uint64_t gen, bool resort) {
    load_index_if_needed();
    bool probed_any = false;
    for (const auto& p : paths) {
        if (tag_cancel_.load(std::memory_order_acquire)) return;
        std::error_code ec;
        auto wt               = std::filesystem::last_write_time(p, ec);
        const std::int64_t mt = ec ? 0 : (std::int64_t)wt.time_since_epoch().count();
        const std::string key = path_utf8(p);
        {
            std::scoped_lock il{index_mu_};
            auto it = index_.find(key);
            if (it != index_.end() && it->second.mtime == mt) continue;
        }
        ProbedMetadata m = probe_metadata(ff, p, worker_);
        TrackMeta tm;
        tm.mtime        = mt;
        tm.album        = std::move(m.album);
        tm.album_artist = std::move(m.album_artist);
        tm.title        = std::move(m.title);
        tm.artist       = std::move(m.artist);
        tm.track_no     = m.track_no;
        tm.disc_no      = m.disc_no;
        tm.duration_ms  = m.duration_ms;
        {
            std::scoped_lock il{index_mu_};
            index_[key] = std::move(tm);
        }
        probed_any = true;
    }
    if (tag_cancel_.load(std::memory_order_acquire)) return;
    if (probed_any) save_index();
    // Let the dashboard know richer titles are available, even when we don't
    // reorder (e.g. shuffle / name stations).
    index_version_.fetch_add(1, std::memory_order_acq_rel);
    if (!resort) return;

    auto ordered = order_album_by_tags(paths);

    std::scoped_lock lk{mu_};
    if (tag_cancel_.load(std::memory_order_acquire) ||
        rebuild_gen_.load(std::memory_order_acquire) != gen)
        return; // a newer rebuild superseded this pass
    std::filesystem::path cur =
        (dec_ && cursor_ < playlist_.size()) ? playlist_[cursor_] : std::filesystem::path{};
    playlist_ = std::move(ordered);
    discard_prefetch_locked();
    if (!cur.empty()) {
        auto it = std::ranges::find(playlist_, cur);
        cursor_ = (it != playlist_.end()) ? (std::size_t)(it - playlist_.begin()) : 0;
    } else {
        cursor_ = 0;
    }
    log::info("[local] album tag index complete; re-sorted {} tracks", playlist_.size());
}

void LocalFileSource::close_current() {
    std::scoped_lock lk{mu_};
    discard_prefetch_locked();
    dec_.reset();
}

std::unique_ptr<LocalFileSource::Decoder> LocalFileSource::open_decoder_locked(std::size_t index) {
    if (playlist_.empty()) return nullptr;
    const std::size_t resolved = index % playlist_.size();
    const auto& path           = playlist_[resolved];

    auto d        = std::make_unique<Decoder>();
    d->for_cursor = resolved;

    const std::wstring ff = ffmpeg_path_.empty() ? L"ffmpeg" : ffmpeg_path_.wstring();
    auto meta      = probe_metadata(ff, path, worker_);
    d->info.duration_ms = meta.duration_ms;
    d->info.title       = std::move(meta.title);
    d->info.artist      = std::move(meta.artist);
    d->info.album       = std::move(meta.album);
    d->art              = extract_cover(ff, path, worker_);

    ma_decoder_config mc = ma_decoder_config_init(ma_format_s16, 2, kSampleRate);
    if (ma_decoder_init_file(path.string().c_str(), &mc, &d->ma) == MA_SUCCESS) {
        d->ma_open = true;
        // Decoded frame count beats ffmpeg's header duration on VBR MP3s.
        ma_uint64 frames = 0;
        if (ma_decoder_get_length_in_pcm_frames(&d->ma, &frames) == MA_SUCCESS)
            d->info.duration_ms = (frames * 1000ull) / kSampleRate;
    } else if (!open_track_ffmpeg(*d, path)) {
        log::warn("[local] failed to open {} (miniaudio rejected the format and the ffmpeg "
                  "fallback also failed; install ffmpeg or convert the file)",
                  path.string());
        return nullptr;
    }

    if (d->info.title.empty()) d->info.title = path_utf8(path.stem());
    if (d->info.album.empty()) d->info.album = path_utf8(path.parent_path().filename());

    float gain_db = std::numeric_limits<float>::quiet_NaN();
    float peak    = std::numeric_limits<float>::quiet_NaN();
    parse_replaygain_file(path, gain_db, peak);
    d->loudness_coef = compute_loudness_correction(gain_db, peak);

    return d;
}

bool LocalFileSource::open_track(std::size_t index) {
    auto d = open_decoder_locked(index);
    if (!d) return false;
    cursor_ = d->for_cursor;
    dec_    = std::move(d);
    position_ms_.store(0, std::memory_order_release);
    log::info("[local] now playing: {}", playlist_[cursor_].string());
    return true;
}

bool LocalFileSource::open_track_ffmpeg(Decoder& d, const std::filesystem::path& path) {
    const std::wstring ff = ffmpeg_path_.empty() ? L"ffmpeg" : ffmpeg_path_.wstring();

    std::wstring cmd =
        quote(ff) + L" -hide_banner -loglevel error -nostdin -vn -i " + quote(path.wstring()) +
        L" -f s16le -acodec pcm_s16le -ar 48000 -ac 2 pipe:1";

    if (worker_ && worker_->alive()) {
        if (auto result = worker_->spawn_single(cmd); result.ok) {
            d.worker      = worker_;
            d.pipeline_id = result.pipeline_id;
            d.ff_pipe     = result.pcm_pipe;
            return true;
        }
        log::warn("[local] worker spawn failed for {} -- falling back to direct spawn",
                  path.string());
    }

    d.ff_job = create_kill_on_close_job();
    if (!d.ff_job) {
        log::warn("[local] ffmpeg fallback: CreateJobObject failed ({})", GetLastError());
        return false;
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul_in  = open_nul(GENERIC_READ);
    HANDLE err_log = open_stderr_log();

    d.ff_proc      = spawn_in_job(d.ff_job, cmd, nul_in, wr, err_log);
    const DWORD ec = d.ff_proc ? 0u : GetLastError();
    CloseHandle(wr);
    if (nul_in) CloseHandle(nul_in);
    if (err_log) CloseHandle(err_log);
    if (!d.ff_proc) {
        CloseHandle(rd);
        log::warn("[local] ffmpeg fallback: failed to launch -- {}",
                  describe_launch_failure(ff, ec, !ffmpeg_path_.empty()));
        return false; // ~Decoder reaps the job
    }

    d.ff_pipe = rd;
    return true;
}

void LocalFileSource::discard_prefetch_locked() noexcept { prefetch_dec_.reset(); }

std::size_t LocalFileSource::next_cursor_locked() const noexcept {
    if (playlist_.empty()) return 0;
    return (cursor_ + 1) % playlist_.size();
}

// Explicit skip/next: always moves forward regardless of repeat. Reshuffles the
// bag on wrap (shuffle order) so each pass is a fresh permutation that never
// repeats the just-finished track first.
bool LocalFileSource::advance_forward_locked() {
    if (playlist_.empty()) return false;
    const bool wrap = (cursor_ + 1 >= playlist_.size());
    if (wrap && is_shuffle_locked() && playlist_.size() > 1) {
        last_played_ = playlist_[cursor_];
        shuffle_in_place_locked(playlist_);
        if (playlist_.front() == last_played_)
            std::swap(playlist_.front(), playlist_[playlist_.size() / 2]);
        discard_prefetch_locked();
        cursor_ = 0;
        return open_track(0);
    }
    const std::size_t n = (cursor_ + 1) % playlist_.size();
    if (promote_prefetch_locked(n)) return true;
    return open_track(n);
}

// Natural end-of-track: honors repeat (one = replay, off = stop at the end,
// all = wrap, reshuffling the bag when in shuffle order).
bool LocalFileSource::eof_advance_locked() {
    if (playlist_.empty()) return false;
    const std::string rep = repeat_mode_locked();
    if (rep == "one") return open_track(cursor_);
    if (rep == "off" && cursor_ + 1 >= playlist_.size()) return false;
    return advance_forward_locked();
}

bool LocalFileSource::promote_prefetch_locked(std::size_t expected_cursor) {
    if (!prefetch_dec_ || prefetch_dec_->for_cursor != expected_cursor) {
        discard_prefetch_locked();
        return false;
    }
    dec_    = std::move(prefetch_dec_);
    cursor_ = dec_->for_cursor;
    position_ms_.store(0, std::memory_order_release);
    log::info("[local] now playing (prebuffered): {}", playlist_[cursor_].string());
    return true;
}

void LocalFileSource::maybe_spawn_prefetch_locked() {
    if (!prebuffer_next_.load(std::memory_order_acquire)) return;
    if (prefetch_dec_ || !dec_ || playlist_.size() < 2) return;
    // Local files (esp. miniaudio) open near-instantly, so no byte threshold is
    // needed -- spawning on the first pump tick after a track change is cheap.
    prefetch_dec_ = open_decoder_locked(next_cursor_locked());
}

void LocalFileSource::play() {
    std::scoped_lock lk{mu_};
    if ((!dec_ || !dec_->any_open()) && !open_track(cursor_)) {
        state_.store(PlaybackState::stopped, std::memory_order_release);
        return;
    }
    state_.store(PlaybackState::playing, std::memory_order_release);
}

void LocalFileSource::pause() { state_.store(PlaybackState::paused, std::memory_order_release); }

bool LocalFileSource::skip_next() {
    std::scoped_lock lk{mu_};
    if (!advance_forward_locked()) return false;
    state_.store(PlaybackState::playing, std::memory_order_release);
    return true;
}

bool LocalFileSource::restart_current() {
    std::scoped_lock lk{mu_};
    if (!dec_ || !dec_->any_open()) return false;
    if (dec_->ma_open) {
        if (ma_decoder_seek_to_pcm_frame(&dec_->ma, 0) != MA_SUCCESS) return false;
        dec_->ma_eof = false;
    } else {
        // ffmpeg pipe is forward-only: re-open the same track from t=0.
        if (!open_track(cursor_)) return false;
    }
    position_ms_.store(0, std::memory_order_release);
    state_.store(PlaybackState::playing, std::memory_order_release);
    return true;
}

void LocalFileSource::stop() {
    close_current();
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

void LocalFileSource::next() {
    std::scoped_lock lk{mu_};
    if (advance_forward_locked()) state_.store(PlaybackState::playing, std::memory_order_release);
}

void LocalFileSource::previous() {
    std::scoped_lock lk{mu_};
    if (playlist_.empty()) return; // size()-1 would underflow size_t
    discard_prefetch_locked();     // prefetch targets cursor+1; previous() rewinds
    open_track(cursor_ == 0 ? playlist_.size() - 1 : cursor_ - 1);
    state_.store(PlaybackState::playing, std::memory_order_release);
}

void LocalFileSource::pump(RingBuffer& ring) {
    if (state_.load(std::memory_order_acquire) != PlaybackState::playing) return;

    std::scoped_lock lk{mu_};
    if (!dec_ || !dec_->any_open()) return;

    const float rg      = volume_norm_.load(std::memory_order_acquire) ? dec_->loudness_coef : 1.0f;
    auto advance_at_eof = [&] {
        if (!eof_advance_locked()) state_.store(PlaybackState::stopped, std::memory_order_release);
    };

    auto apply_dsp = [&](int16_t* samples, std::size_t frames) {
        if (rg != 1.0f) {
            const std::size_t n = frames * 2;
            for (std::size_t i = 0; i < n; ++i) {
                float s = static_cast<float>(samples[i]) * rg;
                if (s > 32767.0f) s = 32767.0f;
                if (s < -32768.0f) s = -32768.0f;
                samples[i] = static_cast<int16_t>(s);
            }
        }
        eq_.process(samples, frames);
    };

    if (dec_->ma_open) {
        // if end of the file, wait for the ring buffer to drain
        if (dec_->ma_eof) {
            ma_uint64 cursor = 0;
            if (ma_decoder_get_cursor_in_pcm_frames(&dec_->ma, &cursor) == MA_SUCCESS) {
                const uint64_t queued = ring.readable() / kFrameBytes;
                const uint64_t played = cursor > queued ? cursor - queued : 0;
                position_ms_.store((played * 1000ull) / kSampleRate, std::memory_order_release);
            }

            if (ring.readable() == 0) advance_at_eof();
            return;
        }

        constexpr std::size_t kChunkFrames = 4096;
        while (ring.writable() >= kChunkFrames * kFrameBytes) {
            int16_t scratch[kChunkFrames * 2];
            ma_uint64 read = 0;
            if (ma_decoder_read_pcm_frames(&dec_->ma, scratch, kChunkFrames, &read) != MA_SUCCESS)
                read = 0;
            if (read == 0) {
                dec_->ma_eof = true;
                break;
            }
            apply_dsp(scratch, static_cast<std::size_t>(read));
            ring.write(scratch, read * kFrameBytes);
            if (read < kChunkFrames) {
                dec_->ma_eof = true;
                break;
            }
        }

        // Audible head, not decoder head: subtract what's still queued in the ring.
        ma_uint64 cursor = 0;
        if (ma_decoder_get_cursor_in_pcm_frames(&dec_->ma, &cursor) == MA_SUCCESS) {
            const uint64_t queued = ring.readable() / kFrameBytes;
            const uint64_t played = cursor > queued ? cursor - queued : 0;
            position_ms_.store((played * 1000ull) / kSampleRate, std::memory_order_release);
        }
        maybe_spawn_prefetch_locked();
        return;
    }

    auto update_position = [&] {
        const std::uint64_t queued = ring.readable();
        const std::uint64_t played = dec_->ff_bytes_out > queued ? dec_->ff_bytes_out - queued : 0;
        position_ms_.store(played * 1000ull / kBytesPerSec, std::memory_order_release);
    };

    if (dec_->ff_eof) {
        update_position();
        if (ring.readable() == 0) advance_at_eof();
        return;
    }

    DWORD avail = 0;
    if (!PeekNamedPipe(dec_->ff_pipe, nullptr, 0, nullptr, &avail, nullptr)) {
        dec_->ff_eof = true;
        update_position();
        return;
    }
    while (avail > 0) {
        const std::size_t writable = ring.writable();
        if (writable < kFrameBytes) break;
        std::size_t want = std::min<std::size_t>(writable, avail);
        if (want > 4096) want = 4096;
        // Force whole stereo S16 frames so the EQ never sees half a sample.
        want &= ~(kFrameBytes - 1);
        if (!want) break;
        std::byte buf[4096];
        DWORD got = 0;
        if (!ReadFile(dec_->ff_pipe, buf, (DWORD)want, &got, nullptr) || got == 0) {
            dec_->ff_eof = true;
            break;
        }
        const DWORD aligned = (got / (DWORD)kFrameBytes) * (DWORD)kFrameBytes;
        if (aligned) {
            apply_dsp(reinterpret_cast<int16_t*>(buf), aligned / kFrameBytes);
            ring.write(buf, aligned);
            dec_->ff_bytes_out += aligned;
        }
        avail = avail > got ? avail - got : 0;
    }
    update_position();
    maybe_spawn_prefetch_locked();
}

TrackInfo LocalFileSource::current_track() const {
    std::scoped_lock lk{mu_};
    TrackInfo info;
    if (dec_) {
        info = dec_->info;
        if (!dec_->art.bytes.empty() && dec_->for_cursor < playlist_.size()) {
            info.artwork_url =
                "/api/artwork?v=" +
                std::to_string(std::hash<std::string>{}(playlist_[dec_->for_cursor].string()));
        }
    }
    info.position_ms = position_ms_.load(std::memory_order_acquire);
    return info;
}

std::optional<ArtworkImage> LocalFileSource::artwork() const {
    std::scoped_lock lk{mu_};
    if (dec_ && !dec_->art.bytes.empty()) return dec_->art;
    return std::nullopt;
}

void LocalFileSource::set_config(LocalFilesConfig cfg) {
    bool rescan;
    bool station_changed;
    {
        std::scoped_lock lk{mu_};
        const LocalStation* o        = active_station_locked();
        const LocalStation old       = o ? *o : LocalStation{};
        const std::string old_active = cfg_.active_station;
        const auto old_formats       = cfg_.supported_formats;
        cfg_                         = std::move(cfg);
        const LocalStation* n        = active_station_locked();
        const LocalStation neu       = n ? *n : LocalStation{};
        station_changed = old_active != cfg_.active_station;
        rescan = old_active != cfg_.active_station || old.roots != neu.roots ||
                 old.excluded != neu.excluded || old.recursive != neu.recursive ||
                 old.order != neu.order || old.grouping != neu.grouping ||
                 old_formats != cfg_.supported_formats;
    }

    if (station_changed) stop();

    if (rescan) rebuild_playlist();
}

void LocalFileSource::set_active_station(std::string name) {
    bool changed = false;
    {
        std::scoped_lock lk{mu_};
        if (cfg_.active_station == name) return;
        cfg_.active_station = std::move(name);
        changed = true;
    }

    if (changed) {
        stop();
        rebuild_playlist();
    }

    rebuild_playlist();
}

void LocalFileSource::reshuffle() {
    std::scoped_lock lk{mu_};
    if (playlist_.size() < 2) return;
    std::filesystem::path cur =
        (dec_ && cursor_ < playlist_.size()) ? playlist_[cursor_] : std::filesystem::path{};
    shuffle_in_place_locked(playlist_);
    discard_prefetch_locked();
    if (!cur.empty()) {
        auto it = std::ranges::find(playlist_, cur);
        cursor_ = (it != playlist_.end()) ? (std::size_t)(it - playlist_.begin()) : 0;
    } else {
        cursor_ = 0;
    }
}

bool LocalFileSource::jump_to(std::size_t index) {
    std::scoped_lock lk{mu_};
    if (index >= playlist_.size()) return false;
    discard_prefetch_locked();
    if (!open_track(index)) return false;
    state_.store(PlaybackState::playing, std::memory_order_release);
    return true;
}

AuthState LocalFileSource::auth_state() const noexcept {
    std::scoped_lock lk{mu_};
    const LocalStation* st = active_station_locked();
    if (!st || st->roots.empty()) return AuthState::needs_auth;
    return playlist_.empty() ? AuthState::needs_auth : AuthState::none_required;
}

std::string LocalFileSource::auth_instructions() const {
    std::scoped_lock lk{mu_};
    const LocalStation* st = active_station_locked();
    if (!st || st->roots.empty())
        return "Add a music folder to this station in the Local Files card, then Save.";
    if (playlist_.empty()) {
        return "No audio files matching the configured formats were found in this station's "
               "folders.";
    }
    return {};
}

std::size_t LocalFileSource::track_count() const noexcept {
    std::scoped_lock lk{mu_};
    return playlist_.size();
}

std::size_t LocalFileSource::station_count() const noexcept {
    std::scoped_lock lk{mu_};
    return cfg_.stations.size();
}

std::string LocalFileSource::active_station_name() const {
    std::scoped_lock lk{mu_};
    const LocalStation* st = active_station_locked();
    return st ? st->name : std::string{};
}

std::string LocalFileSource::active_order() const {
    std::scoped_lock lk{mu_};
    const LocalStation* st = active_station_locked();
    return st ? st->order : std::string{};
}

std::string LocalFileSource::active_grouping() const {
    std::scoped_lock lk{mu_};
    const LocalStation* st = active_station_locked();
    return st ? st->grouping : std::string{};
}

std::string LocalFileSource::active_repeat() const {
    std::scoped_lock lk{mu_};
    const LocalStation* st = active_station_locked();
    return st ? st->repeat : std::string{};
}

void LocalFileSource::set_ffmpeg_path(std::filesystem::path p) {
    std::scoped_lock lk{mu_};
    ffmpeg_path_ = std::move(p);
}

void LocalFileSource::set_playback_options(const PlaybackConfig& opts) {
    volume_norm_.store(opts.volume_normalization, std::memory_order_release);
    eq_.set_options(opts.equalizer_enabled, opts.equalizer_bands, (float)kSampleRate);
    const bool prev =
        prebuffer_next_.exchange(opts.prebuffer_next_track, std::memory_order_acq_rel);
    if (prev && !opts.prebuffer_next_track) {
        std::scoped_lock lk{mu_};
        discard_prefetch_locked();
    }
}

LocalFileSource::QueueSnapshot LocalFileSource::queue_snapshot() const {
    std::scoped_lock lk{mu_};
    std::scoped_lock il{index_mu_};
    QueueSnapshot snap;
    snap.cursor = cursor_;
    snap.entries.reserve(playlist_.size());
    for (std::size_t i = 0; i < playlist_.size(); ++i) {
        const auto& p = playlist_[i];
        QueueEntry e{i, path_utf8(p.stem()), {}, path_utf8(p.parent_path().filename())};
        if (auto it = index_.find(path_utf8(p)); it != index_.end()) {
            if (!it->second.title.empty()) e.title = it->second.title;
            e.artist = it->second.artist;
        }
        snap.entries.push_back(std::move(e));
    }
    return snap;
}

std::vector<FsEntry> enumerate_dir(const std::filesystem::path& dir) {
    std::vector<FsEntry> out;
    if (dir.empty()) {
        const DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if (!(mask & (1u << i))) continue;
            std::string root = std::string(1, char('A' + i)) + ":\\";
            out.push_back(FsEntry{root, root, true});
        }
        return out;
    }
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
        std::error_code de;
        if (!e.is_directory(de)) continue;
        FsEntry fe;
        fe.name = path_utf8(e.path().filename());
        fe.path = path_utf8(e.path());
        // Cheap "has children": stop at the first subdirectory found.
        std::error_code ce;
        std::filesystem::directory_iterator sub(
            e.path(), std::filesystem::directory_options::skip_permission_denied, ce),
            subEnd;
        for (; sub != subEnd && !ce; sub.increment(ce)) {
            std::error_code se;
            if (sub->is_directory(se)) {
                fe.has_children = true;
                break;
            }
        }
        out.push_back(std::move(fe));
    }
    std::ranges::sort(out, [](const FsEntry& a, const FsEntry& b) { return a.name < b.name; });
    return out;
}

} // namespace fh6::sources
