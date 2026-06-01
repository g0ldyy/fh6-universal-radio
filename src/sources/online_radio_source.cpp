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
using subprocess::open_stderr_log;
using subprocess::quote;
using subprocess::spawn_in_job;
using subprocess::widen;

constexpr std::uint64_t kPcmBytesPerSec = 48000ull * 2ull * 2ull;
} // namespace

struct OnlineRadioSource::Pipe {
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
        if (job)       CloseHandle(job);
        if (proc_ff)   CloseHandle(proc_ff);
    }
};

OnlineRadioSource::OnlineRadioSource(OnlineRadioConfig cfg, std::filesystem::path ffmpeg_path)
    : cfg_{std::move(cfg)}, ffmpeg_path_{std::move(ffmpeg_path)} {
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
    if (cfg_.stations.empty()) {
        current_station_idx_ = 0;
    } else if (current_station_idx_ >= cfg_.stations.size()) {
        current_station_idx_ = 0;
    }
}

void OnlineRadioSource::set_ffmpeg_path(std::filesystem::path p) {
    std::scoped_lock lk{mu_};
    ffmpeg_path_ = std::move(p);
}

void OnlineRadioSource::set_target(std::string url) {
    std::scoped_lock lk{mu_};
    target_url_ = std::move(url);
}

void OnlineRadioSource::start_pipe_locked() {
    stop_pipe_locked();
    
    std::string play_url;
    if (!target_url_.empty()) {
        play_url = target_url_;
    } else if (!cfg_.stations.empty()) {
        play_url = cfg_.stations[current_station_idx_].url;
    } else {
        return;
    }

    auto pipe = std::make_unique<Pipe>();
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
    
    if (!CreatePipe(&err_r, &err_w, &sa, 1 << 16)) { CloseHandle(ff_out_r); CloseHandle(ff_out_w); return; }
    SetHandleInformation(err_r, 0, HANDLE_FLAG_INHERIT);

    HANDLE nul_in = open_nul(GENERIC_READ);

    const auto ff = ffmpeg_path_.empty() ? L"ffmpeg" : ffmpeg_path_.wstring();

    // use -loglevel verbose and -icy 1 so ffmpeg emits "ICY Info: StreamTitle='...'"
    std::wstring ff_cmd = quote(ff) + L" -hide_banner -loglevel verbose -icy 1 "
                          L"-reconnect 1 -reconnect_streamed 1 -reconnect_delay_max 5 "
                          L"-i " + quote(widen(play_url)) + L" "
                          L"-f s16le -acodec pcm_s16le -ar 48000 -ac 2 ";
    
    if (volume_norm_.load(std::memory_order_acquire)) {
        ff_cmd += L"-af loudnorm=I=-14:TP=-2:LRA=11 ";
    }
    ff_cmd += L"pipe:1";

    // pass err_w instead of the shared err_log
    pipe->proc_ff = spawn_in_job(pipe->job, ff_cmd, nul_in, ff_out_w, err_w);
    const DWORD ec_ff = pipe->proc_ff ? 0u : GetLastError();
    CloseHandle(ff_out_w);
    CloseHandle(err_w);
    
    if (!pipe->proc_ff) {
        log::warn("[online_radio] failed to launch ffmpeg -- {}", describe_launch_failure(std::wstring{ff}, ec_ff, !ffmpeg_path_.empty()));
        if (ff_out_r) CloseHandle(ff_out_r);
        if (err_r) CloseHandle(err_r);
        if (nul_in) CloseHandle(nul_in);
        return;
    }

    if (nul_in) CloseHandle(nul_in);

    pipe->read_pipe = ff_out_r;
    pipe->stderr_pipe = err_r;
    pipe_ = std::move(pipe);
    
    // set fallback initial metadata
    if (!target_url_.empty()) {
        current_artist_ = "Direct Stream";
        current_title_ = target_url_;
    } else {
        current_artist_ = "Online Radio";
        current_title_ = cfg_.stations[current_station_idx_].name;
    }
    
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
}

void OnlineRadioSource::next() {
    std::scoped_lock lk{mu_};
    target_url_.clear();
    if (cfg_.stations.empty()) return;
    
    current_station_idx_ = (current_station_idx_ + 1) % cfg_.stations.size();
    start_pipe_locked();
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
}

void OnlineRadioSource::previous() {
    std::scoped_lock lk{mu_};
    target_url_.clear();
    if (cfg_.stations.empty()) return;
    
    if (current_station_idx_ == 0) {
        current_station_idx_ = cfg_.stations.size() - 1;
    } else {
        current_station_idx_--;
    }
    start_pipe_locked();
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
}

TrackInfo OnlineRadioSource::current_track() const {
    std::scoped_lock lk{mu_};
    TrackInfo info;
    info.artist = current_artist_;
    info.title = current_title_;
    info.position_ms = position_ms_.load(std::memory_order_acquire);
    return info;
}

void OnlineRadioSource::set_playback_options(const PlaybackConfig& opts) {
    eq_.set_options(opts.equalizer_enabled, opts.equalizer_bands, 48000.0f);
    volume_norm_.store(opts.volume_normalization, std::memory_order_release);
}

void OnlineRadioSource::pump(RingBuffer& ring) {
    auto st = state_.load(std::memory_order_acquire);
    if (st != PlaybackState::playing && st != PlaybackState::buffering) return;

    std::scoped_lock lk{mu_};
    Pipe* p = pipe_.get();
    if (!p) return;
    
    // drain & parse Stderr for ICY Metadata
    if (p->stderr_pipe) {
        DWORD avail = 0;
        while (PeekNamedPipe(p->stderr_pipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            char buf[2048];
            DWORD got = 0;
            if (ReadFile(p->stderr_pipe, buf, std::min<DWORD>(sizeof(buf)-1, avail), &got, nullptr) && got > 0) {
                p->stderr_buf.append(buf, got);
            }
        }
        
        // process line by line
        size_t newline_pos;
        while ((newline_pos = p->stderr_buf.find_first_of("\r\n")) != std::string::npos) {
            std::string line = p->stderr_buf.substr(0, newline_pos);
            // if it's a \r\n sequence, erase both to avoid empty lines
            size_t erase_len = 1;
            if (p->stderr_buf[newline_pos] == '\r' && 
                newline_pos + 1 < p->stderr_buf.size() && 
                p->stderr_buf[newline_pos + 1] == '\n') {
                erase_len = 2;
            }
            p->stderr_buf.erase(0, newline_pos + erase_len);
            
            if (line.empty()) continue;

            std::string payload = line;
            // strip FFmpeg log prefix
            if (!payload.empty() && payload.front() == '[') {
                size_t prefix_end = payload.find("] ");
                if (prefix_end != std::string::npos) {
                    payload = payload.substr(prefix_end + 2);
                }
            }

            // helper to clean up raw values
            auto clean_value = [&](std::string raw) {
                size_t start = 0;
                while (start < raw.size() && (raw[start] == '\'' || raw[start] == '"' || raw[start] == ' ' || raw[start] == '\t')) start++;
                if (start > 0) raw = raw.substr(start);
                while (!raw.empty() && (raw.back() == '\'' || raw.back() == '"' || raw.back() == ';' || raw.back() == ' ' || raw.back() == '\t')) raw.pop_back();
                return raw;
            };

            // catch raw ICY updates which might omit quotes
            size_t icy_idx = payload.find("StreamTitle=");
            if (icy_idx != std::string::npos) {
                size_t val_start = icy_idx + 12;
                size_t val_end = payload.find(';', val_start);
                std::string raw_val = payload.substr(val_start, val_end != std::string::npos ? val_end - val_start : std::string::npos);
                raw_val = clean_value(raw_val);
                
                if (!raw_val.empty()) {
                    std::string new_artist, new_title;
                    auto dash = raw_val.find(" - ");
                    if (dash != std::string::npos && dash > 0) {
                        new_artist = raw_val.substr(0, dash);
                        new_title = raw_val.substr(dash + 3);
                    } else {
                        new_title = raw_val;
                    }
                    
                    // only update and log if the track actually changed
                    if (current_artist_ != new_artist || current_title_ != new_title) {
                        current_artist_ = new_artist;
                        current_title_ = new_title;
                        log::info("[online_radio] Metadata Update (ICY): {} - {}", current_artist_, current_title_);
                    }
                }
                continue;
            }

            // catch standard FFmpeg metadata block entries & ID3 Tags
            size_t colon_idx = payload.find(':');
            if (colon_idx != std::string::npos) {
                std::string key = payload.substr(0, colon_idx);
                size_t k_start = key.find_first_not_of(" \t");
                if (k_start != std::string::npos) {
                    key = key.substr(k_start);
                    size_t k_end = key.find_last_not_of(" \t");
                    if (k_end != std::string::npos) key = key.substr(0, k_end + 1);
                    
                    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
                    
                    std::string raw_val = clean_value(payload.substr(colon_idx + 1));
                    if (raw_val.empty()) continue;

                    bool updated = false;
                    std::string new_artist = current_artist_;
                    std::string new_title = current_title_;

                    if (key.find("streamtitle") != std::string::npos || key.find("icy-name") != std::string::npos) {
                        auto dash = raw_val.find(" - ");
                        if (dash != std::string::npos && dash > 0) {
                            new_artist = raw_val.substr(0, dash);
                            new_title = raw_val.substr(dash + 3);
                        } else {
                            new_artist.clear();
                            new_title = raw_val;
                        }
                        updated = true;
                    } else if (key == "title" || key == "tit2") {
                        auto dash = raw_val.find(" - ");
                        if (dash != std::string::npos && dash > 0) {
                            new_artist = raw_val.substr(0, dash);
                            new_title = raw_val.substr(dash + 3);
                        } else {
                            new_title = raw_val; // for ID3 streams, don't clear the artist tag if one was found
                        }
                        updated = true;
                    } else if (key == "artist" || key == "tpe1") {
                        new_artist = raw_val;
                        updated = true;
                    }

                    // only apply changes and log if the extracted tag is a new song
                    if (updated && (current_artist_ != new_artist || current_title_ != new_title)) {
                        current_artist_ = new_artist;
                        current_title_ = new_title;
                        log::info("[online_radio] Metadata Update (Tag): {} - {}", current_artist_, current_title_);
                    }
                }
            }
        }
        
        // prevent buffer from growing infinitely with incomplete lines
        if (p->stderr_buf.size() > 16384) {
            p->stderr_buf.erase(0, p->stderr_buf.size() - 8192);
        }
    }

    if (!p || !p->read_pipe) return;
    
    auto update_position = [&] {
        const std::size_t r = ring.readable();
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

    // transition back to buffering if starved
    if (avail == 0) {
        if (!p->ended && state_.load(std::memory_order_acquire) == PlaybackState::playing &&
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