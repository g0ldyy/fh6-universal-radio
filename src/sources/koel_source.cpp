#include "fh6/sources/koel_source.hpp"
#include "fh6/log.hpp"
#include "fh6/net/http_get.hpp"
#include "fh6/subprocess.hpp"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace fh6::sources {

namespace {

using subprocess::create_kill_on_close_job;
using subprocess::describe_launch_failure;
using subprocess::open_nul;
using subprocess::open_stderr_log;
using subprocess::quote;
using subprocess::spawn_in_job;
using subprocess::widen;

using json = nlohmann::json;

constexpr std::uint64_t kPcmBytesPerSec = 48000ull * 2ull * 2ull;

bool config_complete(const KoelConfig& c) noexcept {
    return !c.server_url.empty() && !c.password.empty() && !c.source_type.empty();
}

bool same_query_target(const KoelConfig& a, const KoelConfig& b) noexcept {
    return a.server_url == b.server_url && a.password == b.password &&
           a.username == b.username && a.source_type == b.source_type &&
           a.source_id == b.source_id && a.random_count == b.random_count;
}

std::string url_encode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else
            out += std::format("%{:02X}", c);
    }
    return out;
}

std::string md5_hex(const std::string& input) {
    BCRYPT_ALG_HANDLE algo = nullptr;
    if (BCryptOpenAlgorithmProvider(&algo, BCRYPT_MD5_ALGORITHM, nullptr, 0) != 0)
        return {};

    DWORD hash_obj_len = 0, hash_len = 0, unused = 0;
    auto st = BCryptGetProperty(algo, BCRYPT_OBJECT_LENGTH, (PUCHAR)&hash_obj_len,
                                sizeof(hash_obj_len), &unused, 0);
    if (st != 0) { BCryptCloseAlgorithmProvider(algo, 0); return {}; }
    st = BCryptGetProperty(algo, BCRYPT_HASH_LENGTH, (PUCHAR)&hash_len, sizeof(hash_len),
                           &unused, 0);
    if (st != 0) { BCryptCloseAlgorithmProvider(algo, 0); return {}; }

    std::vector<BYTE> hash_obj(hash_obj_len);
    std::vector<BYTE> hash(hash_len);

    BCRYPT_HASH_HANDLE h = nullptr;
    st = BCryptCreateHash(algo, &h, hash_obj.data(), (ULONG)hash_obj.size(), nullptr, 0, 0);
    if (st != 0) { BCryptCloseAlgorithmProvider(algo, 0); return {}; }

    st = BCryptHashData(h, (PUCHAR)input.data(), (ULONG)input.size(), 0);
    if (st != 0) { BCryptDestroyHash(h); BCryptCloseAlgorithmProvider(algo, 0); return {}; }

    st = BCryptFinishHash(h, hash.data(), (ULONG)hash.size(), 0);
    if (st != 0) { BCryptDestroyHash(h); BCryptCloseAlgorithmProvider(algo, 0); return {}; }

    BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(algo, 0);

    std::string out;
    out.reserve(hash_len * 2);
    for (BYTE b : hash)
        out += std::format("{:02x}", b);
    return out;
}

std::string auth_query(const KoelConfig& cfg) {
    const auto& u = cfg.username.empty() ? "subsonic" : cfg.username;
    // Prefer token-based auth (Subsonic / OpenSubsonic standard).
    // Falls back to password-based for servers that don't support tokens.
    static thread_local std::mt19937 rng{std::random_device{}()};
    auto salt = std::to_string(rng());
    auto token = md5_hex(cfg.password + salt);
    if (!token.empty())
        return std::format("u={}&t={}&s={}&v=1.16.1&c=fh6-radio",
                           url_encode(u), token, url_encode(salt));
    return std::format("u={}&p={}&v=1.16.1&c=fh6-radio",
                       url_encode(u), url_encode(cfg.password));
}

std::string api_url(const KoelConfig& cfg, const std::string& method,
                    const std::string& extra_params = {}) {
    std::string url = cfg.server_url;
    if (!url.ends_with('/')) url += '/';
    url += "rest/" + method + ".view?f=json&" + auth_query(cfg);
    if (!extra_params.empty()) url += "&" + extra_params;
    return url;
}

std::string make_stream_url(const KoelConfig& cfg, const std::string& id) {
    std::string url = cfg.server_url;
    if (!url.ends_with('/')) url += '/';
    url += "rest/stream.view?id=" + url_encode(id) + "&" + auth_query(cfg);
    return url;
}

KoelTrack parse_song_entry(const json& entry) {
    KoelTrack t;
    t.id     = entry.value("id", "");
    t.parent = entry.value("parent", "");
    t.title  = entry.value("title", "Unknown Title");
    t.artist = entry.value("artist", "Unknown Artist");
    t.album  = entry.value("album", "");
    t.duration_ms = static_cast<std::uint64_t>(entry.value("duration", 0)) * 1000ull;
    // coverArt may be the song ID or the parent (album) ID – try parent first for getCoverArt
    if (auto ca = entry.find("coverArt"); ca != entry.end() && ca->is_string())
        t.image_url = ca->get<std::string>();
    return t;
}

struct FetchResult {
    std::vector<KoelTrack> tracks;
    std::string error;
};

FetchResult fetch_tracks(const KoelConfig& cfg) {
    std::string method;
    std::string extra_params;
    bool expect_songs_key = true; // true => "song" array; false => "entry" array

    if (cfg.source_type == "favorites") {
        method = "getStarred2";
        expect_songs_key = true;
    } else if (cfg.source_type == "random") {
        method = "getRandomSongs";
        extra_params = "size=" + std::to_string(cfg.random_count);
        expect_songs_key = true;
    } else if (cfg.source_type == "playlist") {
        if (cfg.source_id.empty()) {
            log::error("[koel] playlist source requires source_id");
            return {{}, "playlist source_type requires a source_id"};
        }
        method = "getPlaylist";
        extra_params = "id=" + url_encode(cfg.source_id);
        expect_songs_key = false;
    } else if (cfg.source_type == "album") {
        if (cfg.source_id.empty()) {
            log::error("[koel] album source requires source_id");
            return {{}, "album source_type requires a source_id"};
        }
        method = "getAlbum";
        extra_params = "id=" + url_encode(cfg.source_id);
        expect_songs_key = true;
    } else if (cfg.source_type == "artist") {
        if (cfg.source_id.empty()) {
            log::error("[koel] artist source requires source_id");
            return {{}, "artist source_type requires a source_id"};
        }
        // Multi-step: getArtist -> album list -> getAlbum per album
        auto artist_url = api_url(cfg, "getArtist", "id=" + url_encode(cfg.source_id));
        auto artist_body = net::http_get(artist_url);
        if (!artist_body) {
            log::error("[koel] HTTP request failed for getArtist");
                            return {{}, "HTTP request to Subsonic server failed – check the URL"};
        }

        std::vector<std::string> album_ids;
        try {
            auto root = json::parse(*artist_body);
            auto resp = root.find("subsonic-response");
            if (resp == root.end() || !resp->is_object()) {
                log::error("[koel] getArtist response missing subsonic-response");
                            return {{}, "unexpected response from Subsonic server (missing subsonic-response)"};
            }
            if (resp->value("status", "") != "ok") {
                auto err_obj = resp->value("error", json::object());
                auto msg = err_obj.value("message", "unknown error");
                log::error("[koel] getArtist API error: {}", msg);
                            return {{}, "Subsonic API error: " + msg};
            }
            auto art = resp->find("artist");
            if (art == resp->end() || !art->is_object()) {
                log::error("[koel] getArtist missing artist object");
                            return {{}, "unexpected response from Subsonic server (missing artist)"};
            }
            auto albums = art->find("album");
            if (albums != art->end() && albums->is_array()) {
                for (const auto& alb : *albums) {
                    auto id = alb.value("id", "");
                    if (!id.empty()) album_ids.push_back(std::move(id));
                }
            }
        } catch (const std::exception& e) {
            log::error("[koel] getArtist JSON parse error: {}", e.what());
                            return {{}, "failed to parse Subsonic server response: " + std::string(e.what())};
        }

        if (album_ids.empty()) {
            log::warn("[koel] artist has no albums");
            return {{}, {}};
        }

        std::vector<KoelTrack> out;
        for (const auto& aid : album_ids) {
            auto album_url = api_url(cfg, "getAlbum", "id=" + url_encode(aid));
            auto album_body = net::http_get(album_url);
            if (!album_body) continue;

            try {
                auto root  = json::parse(*album_body);
                auto resp  = root.find("subsonic-response");
                if (resp == root.end() || !resp->is_object()) continue;
                auto album = resp->find("album");
                if (album == resp->end() || !album->is_object()) continue;
                auto songs = album->find("song");
                if (songs == album->end() || !songs->is_array()) continue;
                for (const auto& song : *songs) {
                    auto t = parse_song_entry(song);
                    if (!t.id.empty()) out.push_back(std::move(t));
                }
            } catch (const std::exception& e) {
                log::warn("[koel] getAlbum JSON parse (album {}) : {}", aid, e.what());
            } catch (...) {
                log::warn("[koel] getAlbum JSON parse (album {}) : unknown exception", aid);
            }
        }

        log::info("[koel] fetched {} track(s) for artist", out.size());
        return {std::move(out), {}};
    } else {
        log::error("[koel] unknown source_type '{}'", cfg.source_type);
        return {{}, "unknown source type '" + cfg.source_type + "'"};
    }

    auto url  = api_url(cfg, method, extra_params);
    log::info("[koel] fetching {} (method={})", cfg.server_url + "/rest/" + method + ".view", method);
    auto body = net::http_get(url, "Accept: application/json");
    if (!body) {
        log::error("[koel] HTTP request failed for {}", method);
        log::error("[koel] constructed URL was: {}.view?f=json&u={}&t=...&s=...&v=1.16.1&c=fh6-radio{}",
                   cfg.server_url + "/rest/" + method, url_encode(cfg.username.empty() ? "subsonic" : cfg.username),
                   extra_params.empty() ? "" : "&" + extra_params);
                            return {{}, "cannot reach Subsonic server – check the URL and that the server is running"};
    }

    std::vector<KoelTrack> out;
    try {
        auto root = json::parse(*body);
        auto resp = root.find("subsonic-response");
        if (resp == root.end() || !resp->is_object()) {
            log::error("[koel] response missing subsonic-response");
                            return {{}, "unexpected response from Subsonic server (not a valid Subsonic API response)"};
        }
        if (resp->value("status", "") != "ok") {
            auto err_obj = resp->value("error", json::object());
            auto msg = err_obj.value("message", "unknown error");
            log::error("[koel] API error: {} - {}",
                       err_obj.value("code", 0), msg);
                            return {{}, "Subsonic API error: " + msg};
        }

        const json* container = nullptr;
        {
            auto it = resp->end();
            if (cfg.source_type == "favorites")
                it = resp->find("starred2");
            else if (cfg.source_type == "random")
                it = resp->find("randomSongs");
            else if (cfg.source_type == "playlist")
                it = resp->find("playlist");
            else if (cfg.source_type == "album")
                it = resp->find("album");
            if (it != resp->end() && it->is_object())
                container = &*it;
        }

        if (!container) {
            log::error("[koel] no data container in response");
                            return {{}, "unexpected response from Subsonic server (no " + cfg.source_type + " data)"};
        }

        const json* entries = nullptr;
        {
            auto key   = expect_songs_key ? "song" : "entry";
            auto it    = container->find(key);
            if (it != container->end() && it->is_array()) {
                entries = &*it;
            } else {
                auto other = expect_songs_key ? "entry" : "song";
                it         = container->find(other);
                if (it != container->end() && it->is_array())
                    entries = &*it;
            }
        }

        if (!entries) {
            log::error("[koel] no song/entry array in container");
            return {{}, "no songs found in " + cfg.source_type};
        }

        out.reserve(entries->size());
        for (const auto& entry : *entries) {
            auto t = parse_song_entry(entry);
            if (!t.id.empty()) out.push_back(std::move(t));
        }
    } catch (const std::exception& e) {
        log::error("[koel] JSON parse error: {}", e.what());
                            return {{}, "failed to parse Subsonic server response: " + std::string(e.what())};
    }

    log::info("[koel] fetched {} track(s)", out.size());
    return {std::move(out), {}};
}

void shuffle_range(std::vector<KoelTrack>& q, std::size_t from) {
    if (from >= q.size() || q.size() - from < 2) return;
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::shuffle(q.begin() + (std::ptrdiff_t)from, q.end(), rng);
}

std::mutex& fetch_serializer() {
    static std::mutex m;
    return m;
}

} // namespace

struct KoelSource::Pipe {
    worker::WorkerClient* worker = nullptr;
    uint32_t pipeline_id = 0;

    HANDLE job       = nullptr;
    HANDLE proc      = nullptr;

    HANDLE read_pipe = nullptr;
    std::uint64_t bytes_written = 0;
    std::atomic<std::uint64_t> position_ms{0};
    bool ended                = false;
    std::size_t for_queue_idx = 0;

    ~Pipe() {
        if (read_pipe) { CloseHandle(read_pipe); read_pipe = nullptr; }
        if (worker && pipeline_id) worker->kill_pipeline(pipeline_id);
        subprocess::reap(proc);
        if (job) CloseHandle(job);
    }
};

KoelSource::KoelSource(KoelConfig cfg, std::filesystem::path ffmpeg_path,
                       worker::WorkerClient* worker)
    : cfg_{std::move(cfg)}, ffmpeg_path_{std::move(ffmpeg_path)}, worker_{worker} {}

KoelSource::~KoelSource() {
    std::scoped_lock lk{mu_};
    discard_prefetch_locked();
    stop_pipe_locked();
}

bool KoelSource::initialize() {
    if (!cfg_.enabled) return false;
    if (!config_complete(cfg_)) {
        auth_state_.store(AuthState::needs_auth, std::memory_order_release);
        return true;
    }
    auto result = fetch_tracks(cfg_);
    if (result.error.empty()) {
        auth_state_.store(AuthState::authenticated, std::memory_order_release);
        {
            std::scoped_lock lk{mu_};
            auth_error_.clear();
            queue_ = std::move(result.tracks);
            cached_artwork_idx_ = SIZE_MAX;
            if (cfg_.shuffle) shuffle_range(queue_, 0);
        }
    } else {
        log::error("[koel] initialize: {}", result.error);
        auth_state_.store(AuthState::error, std::memory_order_release);
        std::scoped_lock lk{mu_};
        auth_error_ = result.error;
    }
    return true;
}

void KoelSource::shutdown() noexcept {
    std::scoped_lock lk{mu_};
    discard_prefetch_locked();
    stop_pipe_locked();
}

std::unique_ptr<KoelSource::Pipe> KoelSource::spawn_pipe_locked(std::size_t for_idx) {
    if (queue_.empty() || for_idx >= queue_.size()) return nullptr;

    auto pipe           = std::make_unique<Pipe>();
    pipe->for_queue_idx = for_idx;

    const std::wstring ff = ffmpeg_path_.empty() ? std::wstring{L"ffmpeg"}
                                                  : ffmpeg_path_.wstring();
    const std::string stream_url = make_stream_url(cfg_, queue_[for_idx].id);

    std::wstring cmd = quote(ff) +
        L" -loglevel error -reconnect 1 -reconnect_at_eof 1 -reconnect_streamed 1 "
        L"-reconnect_delay_max 5 -i " + quote(widen(stream_url)) + L" -f s16le ";
    if (volume_norm_.load(std::memory_order_acquire))
        cmd += L"-af loudnorm=I=-14:TP=-2:LRA=11 ";
    cmd += L"-acodec pcm_s16le -ar 48000 -ac 2 pipe:1";

    if (worker_ && worker_->alive()) {
        if (auto result = worker_->spawn_single(cmd); result.ok) {
            pipe->worker      = worker_;
            pipe->pipeline_id = result.pipeline_id;
            pipe->read_pipe   = result.pcm_pipe;
            return pipe;
        }
        log::warn("[koel] worker spawn failed for {} -- falling back to direct spawn",
                  queue_[for_idx].id);
    }

    auto job = create_kill_on_close_job();
    if (!job) {
        log::warn("[koel] CreateJobObject failed ({})", GetLastError());
        return nullptr;
    }
    pipe->job = job;

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out_r = nullptr, out_w = nullptr;
    if (!CreatePipe(&out_r, &out_w, &sa, 1 << 20)) return nullptr;
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul_in  = open_nul(GENERIC_READ);
    HANDLE err_log = open_stderr_log();

    pipe->proc = spawn_in_job(pipe->job, cmd, nul_in, out_w, err_log);
    const DWORD ec = pipe->proc ? 0u : GetLastError();
    CloseHandle(out_w);
    if (nul_in) CloseHandle(nul_in);
    if (err_log) CloseHandle(err_log);
    if (!pipe->proc) {
        CloseHandle(out_r);
        log::warn("[koel] failed to launch ffmpeg -- {}",
                  describe_launch_failure(ff, ec, !ffmpeg_path_.empty()));
        return nullptr;
    }

    pipe->read_pipe = out_r;
    return pipe;
}

void KoelSource::start_pipe_locked() {
    stop_pipe_locked();
    pipe_ = spawn_pipe_locked(current_idx_);
}

void KoelSource::stop_pipe_locked() {
    pipe_.reset();
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

void KoelSource::discard_prefetch_locked() noexcept { prefetch_.reset(); }

std::size_t KoelSource::next_queue_idx_locked() const noexcept {
    if (queue_.empty()) return 0;
    return (current_idx_ + 1) % queue_.size();
}

bool KoelSource::promote_prefetch_locked(std::size_t expected_idx) {
    if (!prefetch_ || prefetch_->for_queue_idx != expected_idx) {
        discard_prefetch_locked();
        return false;
    }
    pipe_ = std::move(prefetch_);
    return true;
}

void KoelSource::maybe_spawn_prefetch_locked() {
    if (!prebuffer_next_.load(std::memory_order_acquire)) return;
    if (prefetch_ || !pipe_ || queue_.size() < 2) return;
    constexpr std::uint64_t kViableBytes = 96 * 1024;
    if (pipe_->bytes_written < kViableBytes) return;
    prefetch_ = spawn_pipe_locked(next_queue_idx_locked());
}

void KoelSource::advance_locked(std::ptrdiff_t step) {
    if (queue_.empty()) return;
    const auto n = (std::ptrdiff_t)queue_.size();
    auto i       = (std::ptrdiff_t)current_idx_ + step;
    current_idx_ = (std::size_t)(((i % n) + n) % n);
    if (step == 1 && promote_prefetch_locked(current_idx_)) {
    } else {
        discard_prefetch_locked();
        start_pipe_locked();
    }
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
}

void KoelSource::play() {
    std::scoped_lock lk{mu_};
    if (queue_.empty()) return;
    if (!pipe_) start_pipe_locked();
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
}

void KoelSource::pause() { state_.store(PlaybackState::paused, std::memory_order_release); }

void KoelSource::stop() {
    std::scoped_lock lk{mu_};
    discard_prefetch_locked();
    stop_pipe_locked();
    current_idx_ = 0;
}

void KoelSource::next() {
    std::scoped_lock lk{mu_};
    advance_locked(+1);
}

void KoelSource::previous() {
    std::scoped_lock lk{mu_};
    advance_locked(-1);
}

std::string KoelSource::cast(std::string source_type, std::string source_id) {
    if (source_type.empty()) return "source_type is empty";
    KoelConfig snap;
    {
        std::scoped_lock lk{mu_};
        snap = cfg_;
    }
    snap.source_type = source_type;
    snap.source_id   = source_id;
    if (!config_complete(snap)) {
        if (snap.server_url.empty())
                return "Subsonic server URL not configured – set it in Settings first";
        if (snap.password.empty())
                return "Subsonic password/API key not configured – set it in Settings first";
            return "incomplete Subsonic configuration";
    }

    FetchResult result;
    {
        std::scoped_lock fetch_lk{fetch_serializer()};
        result = fetch_tracks(snap);
    }
    if (!result.error.empty()) return result.error;

    std::scoped_lock lk{mu_};
    cfg_.source_type = std::move(source_type);
    cfg_.source_id   = std::move(source_id);
    queue_           = std::move(result.tracks);
    current_idx_     = 0;
    cached_artwork_idx_ = SIZE_MAX;
    if (cfg_.shuffle) shuffle_range(queue_, 0);
    discard_prefetch_locked();
    start_pipe_locked();
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
    return {};
}

void KoelSource::set_config(KoelConfig cfg) {
    bool requery, shuffle_flip;
    {
        std::scoped_lock lk{mu_};
        requery      = !same_query_target(cfg_, cfg) && config_complete(cfg);
        shuffle_flip = cfg_.shuffle != cfg.shuffle;
    }

    FetchResult result;
    if (requery) {
        std::scoped_lock fetch_lk{fetch_serializer()};
        result = fetch_tracks(cfg);
    }

    std::scoped_lock lk{mu_};
    const bool was_playing = state_.load(std::memory_order_acquire) == PlaybackState::playing;
    cfg_                   = std::move(cfg);

    if (requery) {
        if (result.error.empty()) {
            auth_state_.store(AuthState::authenticated, std::memory_order_release);
            auth_error_.clear();
            if (!result.tracks.empty()) {
                discard_prefetch_locked();
                stop_pipe_locked();
                queue_       = std::move(result.tracks);
                current_idx_ = 0;
                cached_artwork_idx_ = SIZE_MAX;
                if (cfg_.shuffle) shuffle_range(queue_, 0);
                if (was_playing) {
                    start_pipe_locked();
                    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
                }
            }
        } else {
            log::error("[koel] set_config: {}", result.error);
            auth_state_.store(AuthState::error, std::memory_order_release);
            auth_error_ = result.error;
        }
    } else if (!config_complete(cfg_)) {
        auth_state_.store(AuthState::needs_auth, std::memory_order_release);
        auth_error_.clear();
    }
    if (shuffle_flip && cfg_.shuffle) {
        shuffle_range(queue_, current_idx_ + 1);
        discard_prefetch_locked();
    }
}

void KoelSource::set_ffmpeg_path(std::filesystem::path p) {
    std::scoped_lock lk{mu_};
    ffmpeg_path_ = std::move(p);
}

std::string KoelSource::source_type() const {
    std::scoped_lock lk{mu_};
    return cfg_.source_type;
}

std::size_t KoelSource::track_count() const {
    std::scoped_lock lk{mu_};
    return queue_.size();
}

std::string KoelSource::auth_error() const {
    std::scoped_lock lk{mu_};
    return auth_error_;
}

void KoelSource::set_playback_options(const PlaybackConfig& opts) {
    {
        std::scoped_lock lk{mu_};
        eq_.set_options(opts.equalizer_enabled, opts.equalizer_bands, 48000.0f);
    }
    volume_norm_.store(opts.volume_normalization, std::memory_order_release);
    const bool prev =
        prebuffer_next_.exchange(opts.prebuffer_next_track, std::memory_order_acq_rel);
    if (prev && !opts.prebuffer_next_track) {
        std::scoped_lock lk{mu_};
        discard_prefetch_locked();
    }
}

TrackInfo KoelSource::current_track() const {
    std::scoped_lock lk{mu_};
    TrackInfo info;
    if (queue_.empty() || current_idx_ >= queue_.size()) return info;
    const auto& t    = queue_[current_idx_];
    info.title       = t.title;
    info.artist      = t.artist;
    info.album       = t.album;
    info.duration_ms = t.duration_ms;
    if (!t.image_url.empty() && !cfg_.server_url.empty()) {
        if (current_idx_ != cached_artwork_idx_) {
            auto cover_id = !t.parent.empty() ? t.parent : t.image_url;
            std::string url = cfg_.server_url;
            if (url.ends_with('/')) url.resize(url.size() - 1);
            cached_artwork_url_ = std::format("{}/rest/getCoverArt.view?id={}&{}",
                                              url, url_encode(cover_id), auth_query(cfg_));
            cached_artwork_idx_ = current_idx_;
        }
        info.artwork_url = cached_artwork_url_;
    }
    if (pipe_) info.position_ms = pipe_->position_ms.load(std::memory_order_acquire);
    return info;
}

AuthState KoelSource::auth_state() const noexcept {
    return auth_state_.load(std::memory_order_acquire);
}

void KoelSource::pump(RingBuffer& ring) {
    if (state_.load(std::memory_order_acquire) != PlaybackState::playing) return;

    std::scoped_lock lk{mu_};
    Pipe* p = pipe_.get();
    if (!p) return;

    auto update_position = [&] {
        const std::size_t r        = ring.readable();
        const std::uint64_t played = p->bytes_written > r ? p->bytes_written - r : 0;
        p->position_ms.store(played * 1000ull / kPcmBytesPerSec, std::memory_order_release);
    };
    auto on_eof = [&] {
        if (p->read_pipe) {
            CloseHandle(p->read_pipe);
            p->read_pipe = nullptr;
        }
        p->ended = true;
    };

    if (p->ended) {
        update_position();
        if (ring.readable() == 0) advance_locked(+1);
        return;
    }
    if (!p->read_pipe) return;

    DWORD avail = 0;
    if (!PeekNamedPipe(p->read_pipe, nullptr, 0, nullptr, &avail, nullptr)) {
        on_eof();
        return;
    }
    while (avail > 0) {
        const std::size_t writable = ring.writable();
        if (writable < 4) break;
        std::size_t want = std::min<std::size_t>(writable, avail);
        if (want > 4096) want = 4096;
        want &= ~std::size_t{3};
        if (!want) break;

        std::byte buf[4096];
        DWORD got = 0;
        if (!ReadFile(p->read_pipe, buf, (DWORD)want, &got, nullptr) || got == 0) {
            on_eof();
            break;
        }
        const DWORD aligned = (got / 4u) * 4u;
        if (aligned) eq_.process(reinterpret_cast<int16_t*>(buf), aligned / 4u);
        ring.write(buf, aligned);
        p->bytes_written += aligned;
        avail             = avail > got ? avail - got : 0;
    }
    update_position();
    maybe_spawn_prefetch_locked();
}

} // namespace fh6::sources
