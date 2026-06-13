#include "fh6/sources/tidal_source.hpp"
#include "fh6/log.hpp"
#include "fh6/subprocess.hpp"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
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
using subprocess::narrow;

// DEFAULT_TIDAL_CLIENT_ID and DEFAULT_TIDAL_CLIENT_SECRET are public client ID/secret keys
// retrieved from the open-source python-tidal library. They are widely distributed public
// keys intended to allow community applications to access the TIDAL API out-of-the-box
// and do not represent a credential leak of proprietary secrets.
constexpr std::string_view DEFAULT_TIDAL_CLIENT_ID = "fX2JxdmntZWK0ixT";
constexpr std::string_view DEFAULT_TIDAL_CLIENT_SECRET = "1Nn9AfDAjxrgJFJbKNWLeAyKGVGmINuXPPLHVXAvxAg=";

inline std::string get_effective_client_id(const std::string& custom_id) {
    return custom_id.empty() ? std::string(DEFAULT_TIDAL_CLIENT_ID) : custom_id;
}

inline std::string get_effective_client_secret(const std::string& custom_id, const std::string& custom_secret) {
    return custom_id.empty() ? std::string(DEFAULT_TIDAL_CLIENT_SECRET) : custom_secret;
}

constexpr std::uint64_t kPcmBytesPerSec = 48000ull * 2ull * 2ull;
constexpr int kHttpTimeoutMs = 5000;

struct WinHttpDeleter {
    void operator()(void* h) const noexcept { if (h) WinHttpCloseHandle(h); }
};
using WinHttpHandle = std::unique_ptr<void, WinHttpDeleter>;

struct HttpResponse {
    DWORD status = 0;
    std::string body;
};

// Unified WinHTTP request helper
std::optional<HttpResponse> http_request(
    const std::wstring& wmethod,
    const std::wstring& whost,
    INTERNET_PORT port,
    const std::wstring& wpath,
    const std::string& body = "",
    const std::wstring& extra_headers = L""
) {
    WinHttpHandle session{WinHttpOpen(L"FH6 Universal Radio/1.0",
                                       WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session) return std::nullopt;
    WinHttpSetTimeouts(session.get(), kHttpTimeoutMs, kHttpTimeoutMs,
                       kHttpTimeoutMs, kHttpTimeoutMs);

    WinHttpHandle conn{WinHttpConnect(session.get(), whost.c_str(), port, 0)};
    if (!conn) return std::nullopt;

    WinHttpHandle req{WinHttpOpenRequest(conn.get(), wmethod.c_str(), wpath.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          port == 443 ? WINHTTP_FLAG_SECURE : 0)};
    if (!req) return std::nullopt;

    if (!extra_headers.empty()) {
        WinHttpAddRequestHeaders(req.get(), extra_headers.c_str(), (ULONG)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    }

    const void* body_data = body.empty() ? WINHTTP_NO_REQUEST_DATA : body.data();
    DWORD body_len = body.empty() ? 0 : static_cast<DWORD>(body.size());
    if (!WinHttpSendRequest(req.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            const_cast<void*>(body_data), body_len, body_len, 0) ||
        !WinHttpReceiveResponse(req.get(), nullptr)) {
        log::error("[tidal] HTTP request failed (err {})", GetLastError());
        return std::nullopt;
    }

    DWORD status = 0, status_sz = sizeof(status);
    WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz, WINHTTP_NO_HEADER_INDEX);

    std::string response_body;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.get(), &avail) || avail == 0) break;
        const std::size_t off = response_body.size();
        response_body.resize(off + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req.get(), response_body.data() + off, avail, &got)) break;
        response_body.resize(off + got);
        if (got == 0) break;
    }

    HttpResponse res;
    res.status = status;
    res.body = std::move(response_body);
    return res;
}

std::string base64_decode(const std::string& in) {
    std::string out;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) {
        T["ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[i]] = i;
    }
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) continue;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

std::string url_encode(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(c);
        } else {
            out += std::format("%{:02X}", (unsigned char)c);
        }
    }
    return out;
}

void shuffle_range(std::vector<TidalTrack>& q, std::size_t from) {
    if (from >= q.size() || q.size() - from < 2) return;
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::shuffle(q.begin() + (std::ptrdiff_t)from, q.end(), rng);
}

std::mutex& fetch_serializer() {
    static std::mutex m;
    return m;
}

bool read_line_from_pipe(HANDLE pipe, std::string& line) {
    line.clear();
    char c = 0;
    DWORD got = 0;
    while (ReadFile(pipe, &c, 1, &got, nullptr) && got > 0) {
        if (c == '\n') break;
        line.push_back(c);
    }
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
    return !line.empty() || got > 0;
}

struct TidalEnv {
    std::string access_token;
    std::string refresh_token;
    std::string client_id;
    std::string client_secret;
};

std::string run_tidal_helper(const std::wstring& args, const TidalEnv& env = {}) {
    HANDLE job = create_kill_on_close_job();
    if (!job) {
        log::warn("[tidal] failed to create job for helper");
        return "";
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        CloseHandle(job);
        log::warn("[tidal] failed to create pipe for helper");
        return "";
    }
    SetHandleInformation(rd, 0, HANDLE_FLAG_INHERIT);

    HANDLE nul_in = open_nul(GENERIC_READ);
    HANDLE err_log = open_stderr_log();

    std::wstring cmd = L"python -u fh6-radio\\tidal_helper.py " + args;

    if (!env.access_token.empty()) {
        SetEnvironmentVariableW(L"TIDAL_ACCESS_TOKEN", widen(env.access_token).c_str());
    }
    if (!env.refresh_token.empty()) {
        SetEnvironmentVariableW(L"TIDAL_REFRESH_TOKEN", widen(env.refresh_token).c_str());
    }
    if (!env.client_id.empty()) {
        SetEnvironmentVariableW(L"TIDAL_CLIENT_ID", widen(env.client_id).c_str());
    }
    if (!env.client_secret.empty()) {
        SetEnvironmentVariableW(L"TIDAL_CLIENT_SECRET", widen(env.client_secret).c_str());
    }

    HANDLE proc = spawn_in_job(job, cmd, nul_in, wr, err_log);
    const DWORD ec = proc ? 0u : GetLastError();

    if (!env.access_token.empty()) {
        SetEnvironmentVariableW(L"TIDAL_ACCESS_TOKEN", nullptr);
    }
    if (!env.refresh_token.empty()) {
        SetEnvironmentVariableW(L"TIDAL_REFRESH_TOKEN", nullptr);
    }
    if (!env.client_id.empty()) {
        SetEnvironmentVariableW(L"TIDAL_CLIENT_ID", nullptr);
    }
    if (!env.client_secret.empty()) {
        SetEnvironmentVariableW(L"TIDAL_CLIENT_SECRET", nullptr);
    }
    CloseHandle(wr);
    if (nul_in) CloseHandle(nul_in);
    if (err_log) CloseHandle(err_log);

    if (!proc) {
        CloseHandle(rd);
        CloseHandle(job);
        log::error("[tidal] failed to launch tidal_helper.py (err {}). Make sure python is on PATH.", ec);
        return "";
    }

    std::string out;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0) {
        out.append(buf, got);
    }

    CloseHandle(rd);
    CloseHandle(proc);
    CloseHandle(job);
    return out;
}

} // namespace

struct TidalSource::Pipe {
    HANDLE job       = nullptr;
    HANDLE proc      = nullptr;
    HANDLE read_pipe = nullptr;
    std::uint64_t bytes_written = 0;
    std::atomic<std::uint64_t> position_ms{0};
    bool ended = false;
    std::size_t for_queue_idx = 0;

    ~Pipe() {
        if (read_pipe) CloseHandle(read_pipe);
        if (job)       CloseHandle(job);
        if (proc)      CloseHandle(proc);
    }
};

TidalSource::TidalSource(TidalConfig cfg, std::filesystem::path ffmpeg_path, ConfigStore& store)
    : cfg_{std::move(cfg)}, ffmpeg_path_{std::move(ffmpeg_path)}, store_{store} {
    if (cfg_.access_token.empty() || cfg_.refresh_token.empty()) {
        auth_.store(AuthState::needs_auth, std::memory_order_release);
    } else {
        auth_.store(AuthState::authenticated, std::memory_order_release);
    }
}

TidalSource::~TidalSource() {
    stop_auth_thread();
    std::scoped_lock lk{mu_};
    discard_prefetch_locked();
    stop_pipe_locked();
}

bool TidalSource::initialize() {
    if (!cfg_.enabled) return false;

    // Trigger auth polling or token refresh if already authenticated
    if (auth_.load(std::memory_order_acquire) == AuthState::authenticated) {
        uint64_t now = static_cast<uint64_t>(std::time(nullptr));
        if (cfg_.expiry_time <= now + 300) {
            refresh_access_token();
        }
        // Fetch default playlist tracks
        if (!cfg_.default_playlist.empty()) {
            std::scoped_lock lk{mu_};
            refresh_queue_locked();
        }
    } else {
        // Start device authorization code polling thread
        stop_auth_thread_.store(false, std::memory_order_release);
        auth_thread_ = std::thread(&TidalSource::run_auth_loop, this);
    }

    return true;
}

void TidalSource::shutdown() noexcept {
    stop_auth_thread();
    std::scoped_lock lk{mu_};
    discard_prefetch_locked();
    stop_pipe_locked();
}

void TidalSource::stop_auth_thread() noexcept {
    stop_auth_thread_.store(true, std::memory_order_release);
    if (auth_thread_.joinable()) {
        auth_thread_.join();
    }
}

void TidalSource::play() {
    std::scoped_lock lk{mu_};
    if (auth_.load(std::memory_order_acquire) != AuthState::authenticated) return;
    if (queue_.empty()) return;
    if (!pipe_) start_pipe_locked();
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
}

void TidalSource::pause() {
    state_.store(PlaybackState::paused, std::memory_order_release);
}

void TidalSource::stop() {
    std::scoped_lock lk{mu_};
    discard_prefetch_locked();
    stop_pipe_locked();
    current_idx_ = 0;
}

void TidalSource::next() {
    std::scoped_lock lk{mu_};
    advance_locked(+1);
}

void TidalSource::previous() {
    std::scoped_lock lk{mu_};
    advance_locked(-1);
}

void TidalSource::advance_locked(std::ptrdiff_t step) {
    if (queue_.empty()) return;
    const auto n = (std::ptrdiff_t)queue_.size();
    auto i = (std::ptrdiff_t)current_idx_ + step;
    current_idx_ = (std::size_t)(((i % n) + n) % n);
    if (step == 1 && promote_prefetch_locked(current_idx_)) {
        // Prefetch promoted successfully
    } else {
        discard_prefetch_locked();
        start_pipe_locked();
    }
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
}

bool TidalSource::cast(std::string playlist_id) {
    if (playlist_id.empty() || auth_.load(std::memory_order_acquire) != AuthState::authenticated) return false;

    log::info("[tidal] casting playlist ID '{}'", playlist_id);

    std::scoped_lock lk{mu_};
    cfg_.default_playlist = playlist_id;
    if (refresh_queue_locked()) {
        current_idx_ = 0;
        if (cfg_.shuffle) shuffle_range(queue_, 0);
        discard_prefetch_locked();
        start_pipe_locked();
        if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
        return true;
    }
    return false;
}

void TidalSource::set_config(TidalConfig cfg) {
    std::scoped_lock lk{mu_};
    bool enable_flip = cfg_.enabled != cfg.enabled;
    bool shuffle_flip = cfg_.shuffle != cfg.shuffle;
    bool playlist_change = cfg_.default_playlist != cfg.default_playlist;
    bool credentials_changed = cfg_.client_id != cfg.client_id || cfg_.client_secret != cfg.client_secret;

    cfg_ = std::move(cfg);

    if (cfg_.access_token.empty() || cfg_.refresh_token.empty()) {
        auth_.store(AuthState::needs_auth, std::memory_order_release);
    } else {
        auth_.store(AuthState::authenticated, std::memory_order_release);
    }

    if (enable_flip || playlist_change || credentials_changed) {
        discard_prefetch_locked();
        stop_pipe_locked();
        if (auth_.load(std::memory_order_acquire) == AuthState::authenticated) {
            refresh_queue_locked();
            current_idx_ = 0;
            if (cfg_.shuffle) shuffle_range(queue_, 0);
        } else {
            queue_.clear();
            stop_auth_thread();
            stop_auth_thread_.store(false, std::memory_order_release);
            auth_thread_ = std::thread(&TidalSource::run_auth_loop, this);
        }
    } else if (shuffle_flip && cfg_.shuffle) {
        shuffle_range(queue_, current_idx_ + 1);
        discard_prefetch_locked();
    }
}

void TidalSource::set_ffmpeg_path(std::filesystem::path p) {
    std::scoped_lock lk{mu_};
    ffmpeg_path_ = std::move(p);
}

void TidalSource::set_playback_options(const PlaybackConfig& opts) {
    {
        std::scoped_lock lk{mu_};
        eq_.set_options(opts.equalizer_enabled, opts.equalizer_bands, 48000.0f);
    }
    volume_norm_.store(opts.volume_normalization, std::memory_order_release);
    const bool prev = prebuffer_next_.exchange(opts.prebuffer_next_track,
                                                std::memory_order_acq_rel);
    if (prev && !opts.prebuffer_next_track) {
        std::scoped_lock lk{mu_};
        discard_prefetch_locked();
    }
}

TrackInfo TidalSource::current_track() const {
    std::scoped_lock lk{mu_};
    TrackInfo info;
    if (queue_.empty() || current_idx_ >= queue_.size()) return info;
    const auto& t    = queue_[current_idx_];
    info.title       = t.title;
    info.artist      = t.artist;
    info.album       = t.album;
    info.artwork_url = t.artwork_url;
    info.duration_ms = t.duration_ms;
    if (pipe_) info.position_ms = pipe_->position_ms.load(std::memory_order_acquire);
    return info;
}

std::string TidalSource::auth_instructions() const {
    std::scoped_lock lk{mu_};
    if (auth_.load(std::memory_order_acquire) == AuthState::needs_auth && !auth_verification_uri_.empty()) {
        if (!auth_user_code_.empty()) {
            return std::format("Go to <a href=\"{}\" target=\"_blank\">{}</a> and enter code <strong>{}</strong> to link your TIDAL account.",
                               auth_verification_uri_, auth_verification_uri_, auth_user_code_);
        }
        return std::format("Go to <a href=\"{}\" target=\"_blank\">{}</a> to link your TIDAL account.",
                           auth_verification_uri_, auth_verification_uri_);
    }
    return "TIDAL is not authenticated. Please enable TIDAL in settings to generate an authentication link.";
}

bool TidalSource::refresh_queue_locked() {
    if (cfg_.default_playlist.empty() || auth_.load(std::memory_order_acquire) != AuthState::authenticated) return false;

    // Check token expiry
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    if (cfg_.expiry_time <= now + 300) {
        mu_.unlock();
        refresh_access_token();
        mu_.lock();
    }

    std::wstring args = widen(std::format(
        "playlist {}",
        cfg_.default_playlist
    ));

    std::string raw;
    {
        std::scoped_lock fetch_lk{fetch_serializer()};
        raw = run_tidal_helper(args, TidalEnv{ .access_token = cfg_.access_token });
    }

    if (raw.empty()) {
        log::error("[tidal] playlist helper returned empty output");
        return false;
    }

    try {
        const auto root = nlohmann::json::parse(raw);
        if (root.value("status", "") != "success") {
            log::error("[tidal] playlist helper error: {}", root.value("error", "Unknown error"));
            return false;
        }

        const auto items = root.find("tracks");
        if (items == root.end() || !items->is_array()) {
            log::error("[tidal] playlist helper response missing tracks array");
            return false;
        }

        std::vector<TidalTrack> tracks;
        tracks.reserve(items->size());

        for (const auto& item : *items) {
            TidalTrack t;
            t.id = item.value("id", "");
            t.title = item.value("title", "");
            t.artist = item.value("artist", "");
            t.album = item.value("album", "");
            t.artwork_url = item.value("artwork_url", "");
            t.duration_ms = item.value("duration_ms", 0ull);
            tracks.push_back(t);
        }

        queue_ = std::move(tracks);
        if (cfg_.shuffle && queue_.size() > 1) {
            shuffle_range(queue_, current_idx_ + 1);
        }
        log::info("[tidal] resolved {} track(s) from playlist via helper", queue_.size());
        return true;
    } catch (const std::exception& e) {
        log::error("[tidal] JSON parse error on helper playlist fetch: {} (raw: {})", e.what(), raw);
        return false;
    }
}

std::unique_ptr<TidalSource::Pipe> TidalSource::spawn_pipe_locked(std::size_t for_idx) {
    if (queue_.empty() || for_idx >= queue_.size() || auth_.load(std::memory_order_acquire) != AuthState::authenticated) return nullptr;

    // Check token expiry
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    if (cfg_.expiry_time <= now + 300) {
        mu_.unlock();
        refresh_access_token();
        mu_.lock();
    }

    const std::string track_id = queue_[for_idx].id;
    std::string quality = cfg_.audio_quality;
    if (quality != "LOW" && quality != "HIGH" && quality != "LOSSLESS" && quality != "HI_RES") {
        quality = "HIGH";
    }

    std::wstring args = widen(std::format(
        "stream {} {}",
        track_id,
        quality
    ));

    std::string raw;
    {
        std::scoped_lock fetch_lk{fetch_serializer()};
        raw = run_tidal_helper(args, TidalEnv{ .access_token = cfg_.access_token });
    }

    if (raw.empty()) {
        log::error("[tidal] stream helper returned empty output");
        return nullptr;
    }

    std::string stream_url;
    try {
        const auto root = nlohmann::json::parse(raw);
        if (root.value("status", "") != "success") {
            log::error("[tidal] stream helper error: {}", root.value("error", "Unknown error"));
            return nullptr;
        }

        std::string mime_type = root.value("mime_type", "");
        if (root.contains("urls") && root["urls"].is_array() && !root["urls"].empty()) {
            stream_url = root["urls"][0].get<std::string>();
        } else if (mime_type == "application/dash+xml") {
            stream_url = std::format("https://api.tidal.com/v1/tracks/{}/playbackinfo?playbackMode=STREAM&assetPresentation=FULL&audioQuality={}", track_id, quality);
        }
    } catch (const std::exception& e) {
        log::error("[tidal] JSON parse error on helper stream: {} (raw: {})", e.what(), raw);
        return nullptr;
    }

    if (stream_url.empty()) {
        log::error("[tidal] failed to resolve stream URL for track {}", track_id);
        return nullptr;
    }

    auto pipe = std::make_unique<Pipe>();
    pipe->for_queue_idx = for_idx;
    pipe->job = create_kill_on_close_job();
    if (!pipe->job) {
        log::warn("[tidal] CreateJobObject failed ({})", GetLastError());
        return nullptr;
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out_r = nullptr, out_w = nullptr;
    if (!CreatePipe(&out_r, &out_w, &sa, 1 << 20)) return nullptr;
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul_in = open_nul(GENERIC_READ);
    HANDLE err_log = open_stderr_log();

    const std::wstring ff = ffmpeg_path_.empty() ? std::wstring{L"ffmpeg"} : ffmpeg_path_.wstring();
    const std::wstring auth_hdr = widen(std::format("Authorization: Bearer {}\r\n", cfg_.access_token));

    std::wstring cmd = quote(ff) + L" -loglevel error -headers " + quote(auth_hdr) + L" -reconnect 1 -reconnect_streamed 1 -reconnect_delay_max 5 -i " + quote(widen(stream_url)) + L" -f s16le ";
    if (volume_norm_.load(std::memory_order_acquire)) {
        cmd += L"-af loudnorm=I=-14:TP=-2:LRA=11 ";
    }
    cmd += L"-acodec pcm_s16le -ar 48000 -ac 2 pipe:1";

    pipe->proc = spawn_in_job(pipe->job, cmd, nul_in, out_w, err_log);
    const DWORD ec = pipe->proc ? 0u : GetLastError();
    CloseHandle(out_w);
    if (nul_in) CloseHandle(nul_in);
    if (err_log) CloseHandle(err_log);

    if (!pipe->proc) {
        CloseHandle(out_r);
        log::warn("[tidal] failed to launch ffmpeg -- {}", describe_launch_failure(ff, ec, !ffmpeg_path_.empty()));
        return nullptr;
    }

    pipe->read_pipe = out_r;
    return pipe;
}

void TidalSource::start_pipe_locked() {
    stop_pipe_locked();
    pipe_ = spawn_pipe_locked(current_idx_);
    if (pipe_) {
        state_.store(PlaybackState::playing, std::memory_order_release);
    }
}

void TidalSource::stop_pipe_locked() {
    pipe_.reset();
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

void TidalSource::discard_prefetch_locked() noexcept {
    prefetch_.reset();
}

bool TidalSource::promote_prefetch_locked(std::size_t expected_idx) {
    if (!prefetch_ || prefetch_->for_queue_idx != expected_idx) {
        discard_prefetch_locked();
        return false;
    }
    pipe_ = std::move(prefetch_);
    return true;
}

void TidalSource::maybe_spawn_prefetch_locked() {
    if (!prebuffer_next_.load(std::memory_order_acquire)) return;
    if (prefetch_ || !pipe_ || queue_.size() < 2) return;
    constexpr std::uint64_t kViableBytes = 96 * 1024;
    if (pipe_->bytes_written < kViableBytes) return;
    prefetch_ = spawn_pipe_locked(next_queue_idx_locked());
}

std::size_t TidalSource::next_queue_idx_locked() const noexcept {
    if (queue_.empty()) return 0;
    return (current_idx_ + 1) % queue_.size();
}

void TidalSource::pump(RingBuffer& ring) {
    if (state_.load(std::memory_order_acquire) != PlaybackState::playing) return;

    std::scoped_lock lk{mu_};
    Pipe* p = pipe_.get();
    if (!p) return;

    auto update_position = [&] {
        const std::size_t r = ring.readable();
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
        if (aligned) {
            eq_.process(reinterpret_cast<int16_t*>(buf), aligned / 4u);
        }
        ring.write(buf, aligned);
        p->bytes_written += aligned;
        avail = avail > got ? avail - got : 0;
    }
    update_position();
    maybe_spawn_prefetch_locked();
}

// OAuth2 Thread functions
void TidalSource::run_auth_loop() {
    log::info("[tidal] starting device link auth loop via helper");

    while (!stop_auth_thread_.load(std::memory_order_acquire)) {
        if (auth_.load(std::memory_order_acquire) == AuthState::authenticated) {
            break;
        }

        std::string client_id = get_effective_client_id(cfg_.client_id);
        std::string client_secret = get_effective_client_secret(cfg_.client_id, cfg_.client_secret);

        HANDLE job = create_kill_on_close_job();
        if (!job) {
            Sleep(5000);
            continue;
        }

        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE rd = nullptr, wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 0)) {
            CloseHandle(job);
            Sleep(5000);
            continue;
        }
        SetHandleInformation(rd, 0, HANDLE_FLAG_INHERIT);

        HANDLE nul_in = open_nul(GENERIC_READ);
        HANDLE err_log = open_stderr_log();

        std::wstring helper_cmd = L"python -u fh6-radio\\tidal_helper.py login";

        HANDLE proc = nullptr;
        {
            std::scoped_lock env_lk{fetch_serializer()};
            if (!client_id.empty()) {
                SetEnvironmentVariableW(L"TIDAL_CLIENT_ID", widen(client_id).c_str());
                if (!client_secret.empty()) {
                    SetEnvironmentVariableW(L"TIDAL_CLIENT_SECRET", widen(client_secret).c_str());
                }
            }
            proc = spawn_in_job(job, helper_cmd, nul_in, wr, err_log);
            SetEnvironmentVariableW(L"TIDAL_CLIENT_ID", nullptr);
            SetEnvironmentVariableW(L"TIDAL_CLIENT_SECRET", nullptr);
        }
        const DWORD ec = proc ? 0u : GetLastError();
        CloseHandle(wr);
        if (nul_in) CloseHandle(nul_in);
        if (err_log) CloseHandle(err_log);

        if (!proc) {
            CloseHandle(rd);
            CloseHandle(job);
            log::error("[tidal] failed to launch login helper (err {}). Make sure python is on PATH.", ec);
            for (int k = 0; k < 10 && !stop_auth_thread_.load(std::memory_order_acquire); ++k) {
                Sleep(1000);
            }
            continue;
        }

        std::string first_line;
        if (!read_line_from_pipe(rd, first_line)) {
            log::error("[tidal] helper login exited immediately or failed to print verification details");
            CloseHandle(rd);
            CloseHandle(proc);
            CloseHandle(job);
            Sleep(5000);
            continue;
        }

        std::string verification_uri, user_code;
        try {
            const auto root = nlohmann::json::parse(first_line);
            if (root.contains("error")) {
                log::error("[tidal] helper login error: {}", root.value("error", "Unknown error"));
                CloseHandle(rd);
                CloseHandle(proc);
                CloseHandle(job);
                Sleep(5000);
                continue;
            }
            verification_uri = root.value("verification_uri", "");
            user_code = root.value("user_code", "");
        } catch (const std::exception& e) {
            log::error("[tidal] helper first-line JSON parse error: {} (raw: {})", e.what(), first_line);
            CloseHandle(rd);
            CloseHandle(proc);
            CloseHandle(job);
            Sleep(5000);
            continue;
        }

        {
            std::scoped_lock lk{mu_};
            auth_user_code_ = user_code;
            auth_verification_uri_ = verification_uri;
            auth_.store(AuthState::needs_auth, std::memory_order_release);
        }

        log::info("[tidal] please link your account: {} (code: {})", verification_uri, user_code);

        std::string rest;
        char buf[1024];
        DWORD got = 0;
        while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0) {
            rest.append(buf, got);
        }

        CloseHandle(rd);
        CloseHandle(proc);
        CloseHandle(job);

        if (!rest.empty()) {
            try {
                while (!rest.empty() && (rest.back() == '\r' || rest.back() == '\n' || rest.back() == ' ')) {
                    rest.pop_back();
                }
                const auto root = nlohmann::json::parse(rest);
                if (root.value("status", "") == "success") {
                    std::string access_token;
                    std::string refresh_token;
                    uint64_t expiry_time = 0;
                    {
                        std::scoped_lock lk{mu_};
                        cfg_.access_token = root.value("access_token", "");
                        cfg_.refresh_token = root.value("refresh_token", "");
                        int token_expires_in = root.value("expires_in", 3600);
                        cfg_.expiry_time = static_cast<uint64_t>(std::time(nullptr)) + token_expires_in;
                        auth_.store(AuthState::authenticated, std::memory_order_release);
                        auth_user_code_.clear();
                        auth_verification_uri_.clear();

                        access_token = cfg_.access_token;
                        refresh_token = cfg_.refresh_token;
                        expiry_time = cfg_.expiry_time;
                    }

                    store_.patch([&](Config& c) {
                        c.tidal.access_token = access_token;
                        c.tidal.refresh_token = refresh_token;
                        c.tidal.expiry_time = expiry_time;
                    });

                    log::info("[tidal] account linked successfully via python helper!");
                    {
                        std::scoped_lock lk{mu_};
                        refresh_queue_locked();
                    }
                    break;
                }
 else {
                    log::error("[tidal] account link helper failed: {}", root.value("error", "Unknown error"));
                }
            } catch (const std::exception& e) {
                log::error("[tidal] helper final JSON parse error: {} (raw: {})", e.what(), rest);
            }
        }

        for (int k = 0; k < 5 && !stop_auth_thread_.load(std::memory_order_acquire); ++k) {
            Sleep(1000);
        }
    }
}

bool TidalSource::refresh_access_token() {
    log::info("[tidal] refreshing expired access token via helper");

    std::string client_id = get_effective_client_id(cfg_.client_id);
    std::string client_secret = get_effective_client_secret(cfg_.client_id, cfg_.client_secret);
    if (cfg_.refresh_token.empty()) return false;

    std::wstring args = L"refresh";

    std::string raw;
    {
        std::scoped_lock fetch_lk{fetch_serializer()};
        raw = run_tidal_helper(args, TidalEnv{
            .refresh_token = cfg_.refresh_token,
            .client_id = client_id,
            .client_secret = client_secret
        });
    }

    if (raw.empty()) {
        log::error("[tidal] token refresh helper returned empty output");
        return false;
    }

    try {
        const auto root = nlohmann::json::parse(raw);
        if (root.value("status", "") != "success") {
            log::error("[tidal] token refresh helper error: {}", root.value("error", "Unknown error"));
            {
                std::scoped_lock lk{mu_};
                cfg_.access_token.clear();
                cfg_.refresh_token.clear();
                cfg_.expiry_time = 0;
                auth_.store(AuthState::needs_auth, std::memory_order_release);
                queue_.clear();
            }
            store_.patch([this](Config& c) {
                c.tidal.access_token.clear();
                c.tidal.refresh_token.clear();
                c.tidal.expiry_time = 0;
            });
            stop_auth_thread();
            stop_auth_thread_.store(false, std::memory_order_release);
            auth_thread_ = std::thread(&TidalSource::run_auth_loop, this);
            return false;
        }

        std::string access_token = root.value("access_token", "");
        std::string refresh_token;
        if (root.contains("refresh_token") && !root["refresh_token"].is_null()) {
            refresh_token = root["refresh_token"].get<std::string>();
        }
        int expires_in = root.value("expires_in", 3600);

        std::string final_access_token;
        std::string final_refresh_token;
        uint64_t final_expiry_time = 0;
        {
            std::scoped_lock lk{mu_};
            cfg_.access_token = access_token;
            cfg_.expiry_time = static_cast<uint64_t>(std::time(nullptr)) + expires_in;
            if (!refresh_token.empty()) {
                cfg_.refresh_token = refresh_token;
            }
            final_access_token = cfg_.access_token;
            final_refresh_token = cfg_.refresh_token;
            final_expiry_time = cfg_.expiry_time;
        }

        store_.patch([&](Config& c) {
            c.tidal.access_token = final_access_token;
            c.tidal.refresh_token = final_refresh_token;
            c.tidal.expiry_time = final_expiry_time;
        });

        log::info("[tidal] access token refreshed successfully via helper");
        return true;
    } catch (const std::exception& e) {
        log::error("[tidal] JSON parse error on helper token refresh: {} (raw: {})", e.what(), raw);
        return false;
    }
}

bool TidalSource::exchange_authorization_code(const std::string& code) {
    std::string client_id;
    std::string client_secret;
    {
        std::scoped_lock lk{mu_};
        client_id = get_effective_client_id(cfg_.client_id);
        client_secret = get_effective_client_secret(cfg_.client_id, cfg_.client_secret);
    }
    if (client_id.empty() || client_secret.empty()) {
        log::error("[tidal] cannot exchange authorization code: client_id or client_secret is empty");
        return false;
    }

    // Note: The redirect_uri uses a hardcoded port 8420 which matches the default general port
    // (cfg.general.port). The OAuth client credentials used for the default TIDAL auth flow
    // have this specific URL pre-registered with TIDAL. If the server port is changed in the configuration,
    // authentication may fail unless a different client credentials pair is registered with the matching redirect URI.
    std::string body = std::format(
        "grant_type=authorization_code&code={}&redirect_uri={}&client_id={}&client_secret={}",
        url_encode(code),
        url_encode("http://localhost:8420/api/source/tidal/callback"),
        url_encode(client_id),
        url_encode(client_secret)
    );
    std::wstring headers = L"Content-Type: application/x-www-form-urlencoded\r\n";

    std::optional<HttpResponse> res;
    {
        std::scoped_lock fetch_lk{fetch_serializer()};
        res = http_request(L"POST", L"auth.tidal.com", 443, L"/v1/oauth2/token", body, headers);
    }

    if (!res || res->status != 200) {
        log::error("[tidal] failed to exchange authorization code (status {}): {}", 
                   res ? res->status : 0, 
                   res ? res->body : "No response body");
        return false;
    }

    try {
        const auto root = nlohmann::json::parse(res->body);
        std::string access_token = root.value("access_token", "");
        std::string refresh_token = root.value("refresh_token", "");
        int expires_in = root.value("expires_in", 3600);
        uint64_t expiry_time = static_cast<uint64_t>(std::time(nullptr)) + expires_in;

        if (access_token.empty()) {
            log::error("[tidal] token exchange returned empty access_token");
            return false;
        }

        {
            std::scoped_lock lk{mu_};
            cfg_.access_token = access_token;
            cfg_.refresh_token = refresh_token;
            cfg_.expiry_time = expiry_time;
            auth_.store(AuthState::authenticated, std::memory_order_release);
            auth_verification_uri_.clear();
            auth_user_code_.clear();
        }

        store_.patch([&](Config& c) {
            c.tidal.access_token = access_token;
            c.tidal.refresh_token = refresh_token;
            c.tidal.expiry_time = expiry_time;
        });

        log::info("[tidal] account linked via web authorization successfully!");
        {
            std::scoped_lock lk{mu_};
            refresh_queue_locked();
        }
        return true;
    } catch (const std::exception& e) {
        log::error("[tidal] JSON parse error on token exchange: {}", e.what());
        return false;
    }
}

TidalSource::QueueSnapshot TidalSource::queue_snapshot() const {
    std::scoped_lock lk{mu_};
    QueueSnapshot snap;
    snap.cursor = current_idx_;
    snap.entries = queue_;
    return snap;
}

bool TidalSource::jump_to(std::size_t index) {
    std::scoped_lock lk{mu_};
    if (index >= queue_.size()) return false;
    current_idx_ = index;
    discard_prefetch_locked();
    start_pipe_locked();
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
    return true;
}

std::size_t TidalSource::track_count() const {
    std::scoped_lock lk{mu_};
    return queue_.size();
}

std::size_t TidalSource::current_index() const {
    std::scoped_lock lk{mu_};
    return current_idx_;
}

} // namespace fh6::sources
