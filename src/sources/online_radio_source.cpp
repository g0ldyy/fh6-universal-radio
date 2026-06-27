#include "fh6/sources/online_radio_source.hpp"
#include "fh6/log.hpp"
#include "fh6/subprocess.hpp"

#include <windows.h>
#include <algorithm>

namespace fh6::sources {

namespace {
using subprocess::create_kill_on_close_job;
using subprocess::describe_launch_failure;
using subprocess::open_nul;
using subprocess::quote;
using subprocess::spawn_in_job;
using subprocess::widen;

constexpr std::uint64_t kPcmBytesPerSec = 48000ull * 2ull * 2ull;
} // namespace

struct OnlineRadioSource::Pipe {
    worker::WorkerClient* worker = nullptr;
    uint32_t pipeline_id = 0;

    HANDLE job = nullptr;
    HANDLE proc_ff = nullptr;
    HANDLE read_pipe = nullptr;
    HANDLE stderr_pipe = nullptr;
    std::string stderr_buf;
    std::uint64_t bytes_written = 0;
    bool ended = false;

    ~Pipe() {
        if (read_pipe) CloseHandle(read_pipe);
        if (stderr_pipe) CloseHandle(stderr_pipe);
        if (worker && pipeline_id) worker->kill_pipeline(pipeline_id);
        subprocess::reap(proc_ff); // direct-mode child (no-op in worker mode)
        if (job) CloseHandle(job);
    }
};

bool OnlineRadioSource::is_streamable_url(std::string_view url) noexcept {
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

OnlineRadioSource::OnlineRadioSource(OnlineRadioConfig cfg, std::filesystem::path ffmpeg_path,
                                     worker::WorkerClient* worker)
    : cfg_{std::move(cfg)}, ffmpeg_path_{std::move(ffmpeg_path)}, worker_{worker} {
    if (!cfg_.stations.empty()) {
        current_station_idx_ = std::min(cfg_.default_station_index, cfg_.stations.size() - 1);
    }
}

OnlineRadioSource::~OnlineRadioSource() { stop_pipe_locked(); }

bool OnlineRadioSource::initialize() { return cfg_.enabled; }

void OnlineRadioSource::shutdown() noexcept {
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
}

void OnlineRadioSource::set_config(const OnlineRadioConfig& c) {
    std::scoped_lock lk{mu_};
    cfg_ = c;
    if (current_station_idx_ >= cfg_.stations.size()) current_station_idx_ = 0;
}

void OnlineRadioSource::set_ffmpeg_path(std::filesystem::path p) {
    std::scoped_lock lk{mu_};
    ffmpeg_path_ = std::move(p);
}

void OnlineRadioSource::set_target(std::string url, std::string name, std::string logo) {
    std::scoped_lock lk{mu_};
    target_url_  = std::move(url);
    target_name_ = std::move(name);
    target_logo_ = std::move(logo);
}

void OnlineRadioSource::on_radio_audible(bool audible) {
    std::scoped_lock lk{mu_};
    if (audible == audible_) return;
    audible_ = audible;
    if (audible) {
        log::info("[online_radio] radio audible -- reconnecting to live stream");
        drain_pending_ = true;
        start_pipe_locked();
    } else {
        log::info("[online_radio] radio inaudible -- disconnecting stream");
        pipe_.reset();
    }
}

void OnlineRadioSource::start_pipe_locked() {
    stop_pipe_locked();

    std::string play_url;
    bool is_direct = false;
    if (!target_url_.empty()) {
        play_url  = target_url_;
        is_direct = true;
    } else if (!cfg_.stations.empty()) {
        play_url = cfg_.stations[current_station_idx_].url;
    } else {
        return;
    }

    if (!is_streamable_url(play_url)) {
        log::warn("[online_radio] refusing non-http(s) url: {}", play_url);
        return;
    }

    // initial metadata shown until the stream surfaces ICY/ID3 tags.
    const auto set_initial_meta = [&] {
        if (is_direct) {
            current_title_ = target_name_.empty() ? play_url : target_name_;
            current_logo_  = target_logo_;
        } else {
            const auto& st = cfg_.stations[current_station_idx_];
            current_title_ = st.name;
            current_logo_  = st.favicon;
        }
        current_artist_ = {};
    };

    auto pipe = std::make_unique<Pipe>();

    const auto ff = ffmpeg_path_.empty() ? L"ffmpeg" : ffmpeg_path_.wstring();

    // -loglevel verbose + -icy 1 make ffmpeg emit "ICY Info: StreamTitle='...'".
    std::wstring ff_cmd = quote(ff) + L" -hide_banner -loglevel verbose -icy 1 "
                          L"-reconnect 1 -reconnect_streamed 1 -reconnect_delay_max 5 "
                          L"-i " + quote(widen(play_url)) + L" "
                          L"-f s16le -acodec pcm_s16le -ar 48000 -ac 2 ";
    if (volume_norm_.load(std::memory_order_acquire)) {
        ff_cmd += L"-af loudnorm=I=-14:TP=-2:LRA=11 ";
    }
    ff_cmd += L"pipe:1";

    if (worker_ && worker_->alive()) {
        if (auto result = worker_->spawn_pipeline({ff_cmd}, L"", true); result.ok) {
            pipe->worker      = worker_;
            pipe->pipeline_id = result.pipeline_id;
            pipe->read_pipe   = result.pcm_pipe;
            pipe->stderr_pipe = result.meta_pipe;
            pipe_             = std::move(pipe);

            set_initial_meta();
            position_ms_.store(0, std::memory_order_release);
            state_.store(PlaybackState::buffering, std::memory_order_release);
            log::info("[online_radio] Started stream via worker: {}", play_url);
            return;
        }
        log::warn("[online_radio] worker spawn failed for {} -- falling back to direct spawn", play_url);
    }

    // direct spawn fallback: only executes if the worker is dead/unavailable.
    pipe->job = create_kill_on_close_job();
    if (!pipe->job) {
        log::warn("[online_radio] CreateJobObject failed");
        return;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE ff_out_r = nullptr, ff_out_w = nullptr;
    HANDLE err_r = nullptr, err_w = nullptr;

    if (!CreatePipe(&ff_out_r, &ff_out_w, &sa, 1 << 20)) return;
    SetHandleInformation(ff_out_r, 0, HANDLE_FLAG_INHERIT);

    if (!CreatePipe(&err_r, &err_w, &sa, 1 << 16)) {
        CloseHandle(ff_out_r);
        CloseHandle(ff_out_w);
        return;
    }
    SetHandleInformation(err_r, 0, HANDLE_FLAG_INHERIT);
    HANDLE nul_in = open_nul(GENERIC_READ);

    pipe->proc_ff     = spawn_in_job(pipe->job, ff_cmd, nul_in, ff_out_w, err_w);
    const DWORD ec_ff = pipe->proc_ff ? 0u : GetLastError();
    CloseHandle(ff_out_w);
    CloseHandle(err_w);

    if (!pipe->proc_ff) {
        log::warn("[online_radio] failed to launch ffmpeg -- {}",
                  describe_launch_failure(std::wstring{ff}, ec_ff, !ffmpeg_path_.empty()));
        CloseHandle(ff_out_r);
        CloseHandle(err_r);
        if (nul_in) CloseHandle(nul_in);
        return;
    }

    if (nul_in) CloseHandle(nul_in);

    pipe->read_pipe   = ff_out_r;
    pipe->stderr_pipe = err_r;
    pipe_             = std::move(pipe);

    set_initial_meta();
    position_ms_.store(0, std::memory_order_release);
    state_.store(PlaybackState::buffering, std::memory_order_release);
    log::info("[online_radio] Started stream: {}", play_url);
}

void OnlineRadioSource::stop_pipe_locked() {
    pipe_.reset();
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

void OnlineRadioSource::play() {
    std::scoped_lock lk{mu_};
    drain_pending_ = true;
    if (!pipe_) start_pipe_locked();
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
}

void OnlineRadioSource::pause() {
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
}

void OnlineRadioSource::stop() {
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
    target_url_.clear();
    target_name_.clear();
    target_logo_.clear();
}

void OnlineRadioSource::next() {
    std::scoped_lock lk{mu_};
    target_url_.clear();
    if (cfg_.stations.empty()) return;

    current_station_idx_ = (current_station_idx_ + 1) % cfg_.stations.size();
    if (audible_) {
        start_pipe_locked();
        if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
    }
}

void OnlineRadioSource::previous() {
    std::scoped_lock lk{mu_};
    target_url_.clear();
    if (cfg_.stations.empty()) return;

    current_station_idx_ =
        (current_station_idx_ == 0 ? cfg_.stations.size() : current_station_idx_) - 1;
    if (audible_) {
        start_pipe_locked();
        if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
    }
}

TrackInfo OnlineRadioSource::current_track() const {
    std::scoped_lock lk{mu_};
    TrackInfo info;
    info.artist      = current_artist_;
    info.title       = current_title_;
    info.artwork_url = current_logo_;
    info.position_ms = position_ms_.load(std::memory_order_acquire);
    return info;
}

std::vector<std::string> OnlineRadioSource::song_history() const {
    std::scoped_lock lk{mu_};
    return {song_history_.begin(), song_history_.end()};
}

void OnlineRadioSource::set_playback_options(const PlaybackConfig& opts) {
    {
        std::scoped_lock lk{mu_};
        eq_.set_options(opts.equalizer_enabled, opts.equalizer_bands, 48000.0f);
    }
    // loudnorm lives in the ffmpeg argv; the new state takes effect on the next stream.
    volume_norm_.store(opts.volume_normalization, std::memory_order_release);
}

void OnlineRadioSource::pump(RingBuffer& ring) {
    std::scoped_lock lk{mu_};

    if (drain_pending_) {
        ring.drain();
        drain_pending_ = false;
    }

    auto st = state_.load(std::memory_order_relaxed);
    if (st != PlaybackState::playing && st != PlaybackState::buffering) return;

    Pipe* p = pipe_.get();
    if (!p) return;

    // drain & parse stderr for ICY/ID3 metadata
    if (p->stderr_pipe) {
        DWORD avail = 0;
        int safety  = 0; // bound log reads per cycle so verbose ffmpeg can't starve the audio thread
        while (safety++ < 16 &&
               PeekNamedPipe(p->stderr_pipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            char buf[2048];
            DWORD got = 0;
            if (!ReadFile(p->stderr_pipe, buf, std::min<DWORD>(sizeof(buf) - 1, avail), &got, nullptr) ||
                got == 0)
                break;
            p->stderr_buf.append(buf, got);
        }

        // strip surrounding quotes/separators/whitespace from a raw tag value
        const auto clean_value = [](std::string raw) {
            size_t start = 0;
            while (start < raw.size() &&
                   (raw[start] == '\'' || raw[start] == '"' || raw[start] == ' ' || raw[start] == '\t'))
                ++start;
            if (start > 0) raw = raw.substr(start);
            while (!raw.empty() && (raw.back() == '\'' || raw.back() == '"' || raw.back() == ';' ||
                                    raw.back() == ' ' || raw.back() == '\t'))
                raw.pop_back();
            return raw;
        };

        // split "Artist - Title"; returns true when the separator was present.
        // when absent, only `title` is written (the caller decides the artist).
        const auto split_dash = [](const std::string& raw, std::string& artist, std::string& title) {
            auto dash = raw.find(" - ");
            if (dash != std::string::npos && dash > 0) {
                artist = raw.substr(0, dash);
                title  = raw.substr(dash + 3);
                return true;
            }
            title = raw;
            return false;
        };

        // commit metadata, logging only on an actual change
        const auto apply_meta = [&](const std::string& artist, const std::string& title,
                                    const char* kind) {
            if (current_artist_ == artist && current_title_ == title) return;
            current_artist_ = artist;
            current_title_  = title;
            if (!title.empty()) {
                std::string entry = artist.empty() ? title : artist + " — " + title;
                if (song_history_.empty() || song_history_.front() != entry) {
                    song_history_.push_front(std::move(entry));
                    if (song_history_.size() > 12) song_history_.pop_back();
                }
            }
            log::info("[online_radio] Metadata Update ({}): {} - {}", kind, current_artist_,
                      current_title_);
        };

        size_t newline_pos;
        while ((newline_pos = p->stderr_buf.find_first_of("\r\n")) != std::string::npos) {
            std::string line = p->stderr_buf.substr(0, newline_pos);
            // collapse a \r\n pair so it doesn't leave an empty line behind
            size_t erase_len = 1;
            if (p->stderr_buf[newline_pos] == '\r' && newline_pos + 1 < p->stderr_buf.size() &&
                p->stderr_buf[newline_pos + 1] == '\n') {
                erase_len = 2;
            }
            p->stderr_buf.erase(0, newline_pos + erase_len);
            if (line.empty()) continue;

            std::string payload = std::move(line);
            // strip the "[component] " ffmpeg log prefix
            if (payload.front() == '[') {
                size_t prefix_end = payload.find("] ");
                if (prefix_end != std::string::npos) payload = payload.substr(prefix_end + 2);
            }

            // raw ICY update (may omit quotes): StreamTitle=...;
            if (size_t icy_idx = payload.find("StreamTitle="); icy_idx != std::string::npos) {
                size_t val_start = icy_idx + 12;
                size_t val_end   = payload.find(';', val_start);
                std::string raw  = clean_value(payload.substr(
                    val_start, val_end != std::string::npos ? val_end - val_start : std::string::npos));
                if (!raw.empty()) {
                    std::string artist, title;
                    split_dash(raw, artist, title);
                    apply_meta(artist, title, "ICY");
                }
                continue;
            }

            // standard ffmpeg metadata block entries & ID3 tags: "key : value"
            size_t colon_idx = payload.find(':');
            if (colon_idx == std::string::npos) continue;

            std::string key = payload.substr(0, colon_idx);
            size_t k_start  = key.find_first_not_of(" \t");
            if (k_start == std::string::npos) continue;
            size_t k_end = key.find_last_not_of(" \t");
            key          = key.substr(k_start, k_end - k_start + 1);
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            std::string raw = clean_value(payload.substr(colon_idx + 1));
            if (raw.empty()) continue;

            if (key.find("streamtitle") != std::string::npos ||
                key.find("icy-name") != std::string::npos) {
                std::string artist, title;
                split_dash(raw, artist, title);
                apply_meta(artist, title, "Tag");
            } else if (key == "title" || key == "tit2") {
                // keep any artist already parsed when this tag has no "Artist - Title" form
                std::string artist = current_artist_, title;
                split_dash(raw, artist, title);
                apply_meta(artist, title, "Tag");
            } else if (key == "artist" || key == "tpe1") {
                apply_meta(raw, current_title_, "Tag");
            }
        }

        // cap the backlog so a stream of incomplete lines can't grow unbounded
        if (p->stderr_buf.size() > 16384) {
            p->stderr_buf.erase(0, p->stderr_buf.size() - 8192);
        }
    }

    if (!p->read_pipe) return;

    const auto update_position = [&] {
        const std::size_t r        = ring.readable();
        const std::uint64_t played = p->bytes_written > r ? p->bytes_written - r : 0;
        position_ms_.store(played * 1000ull / kPcmBytesPerSec, std::memory_order_release);
    };

    if (p->ended) {
        update_position();
        return;
    }

    DWORD avail = 0;
    if (!PeekNamedPipe(p->read_pipe, nullptr, 0, nullptr, &avail, nullptr)) {
        p->ended = true;
        CloseHandle(p->read_pipe);
        p->read_pipe = nullptr;
        return;
    }

    // transition back to buffering if the stream starved
    if (avail == 0) {
        if (state_.load(std::memory_order_acquire) == PlaybackState::playing &&
            ring.readable() < 16384) {
            state_.store(PlaybackState::buffering, std::memory_order_release);
        }
        update_position();
        return;
    }

    while (avail > 0) {
        const std::size_t writable = ring.writable();
        if (writable < 4) break;

        std::size_t want = std::min<std::size_t>(writable, avail);
        if (want > 4096) want = 4096;
        want &= ~std::size_t{3}; // 4-byte align (16-bit stereo)
        if (!want) break;

        std::byte buf[4096];
        DWORD got = 0;
        if (!ReadFile(p->read_pipe, buf, (DWORD)want, &got, nullptr) || got == 0) {
            p->ended = true;
            CloseHandle(p->read_pipe);
            p->read_pipe = nullptr;
            return;
        }

        const DWORD aligned = (got / 4u) * 4u;
        if (aligned) eq_.process(reinterpret_cast<int16_t*>(buf), aligned / 4u);

        ring.write(buf, aligned);
        p->bytes_written += aligned;
        update_position();

        avail = avail > got ? avail - got : 0;

        if (state_.load(std::memory_order_acquire) == PlaybackState::buffering &&
            ring.readable() >= 384 * 1024) { // ~2 seconds of pre-buffer
            state_.store(PlaybackState::playing, std::memory_order_release);
        }
    }
    update_position();
}

} // namespace fh6::sources
