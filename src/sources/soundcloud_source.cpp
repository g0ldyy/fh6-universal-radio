#include "fh6/sources/soundcloud_source.hpp"
#include "fh6/log.hpp"
#include "fh6/subprocess.hpp"
#include "fh6/net/http_get.hpp"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>
#include <chrono>
#include <regex>
#include <optional>
#include <fstream>
#include <filesystem>

namespace fh6::sources {

namespace {

using subprocess::create_kill_on_close_job;
using subprocess::describe_launch_failure;
using subprocess::drain_to_eof;
using subprocess::open_nul;
using subprocess::open_stderr_log;
using subprocess::quote;
using subprocess::spawn_in_job;
using subprocess::stderr_log_path;
using subprocess::widen;

// PCM contract written by ffmpeg: 48000 Hz * 2 ch * 2 bytes.
constexpr std::uint64_t kPcmBytesPerSec = 48000ull * 2ull * 2ull;

bool is_playlist_url(std::string_view url) {
    // SoundCloud playlists typically contain /sets/ or /likes
    return url.find("/sets/") != std::string_view::npos ||
           url.find("/likes") != std::string_view::npos;
}

std::optional<std::string> worker_http_get(worker::WorkerClient* worker, const std::string& url) {
    if (!worker || !worker->alive()) {
        return fh6::net::http_get(url); // fallback to direct fetch
    }

    static std::atomic<uint64_t> fetch_id{0};
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::string temp_path = (temp_dir / ("fh6_sc_tmp_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(++fetch_id))).string();

    // ensure the file is deleted upon exit
    struct FileCleanupGuard {
        std::string p;
        ~FileCleanupGuard() {
            std::error_code ec;
            if (!p.empty()) std::filesystem::remove(p, ec);
        }
    } guard{temp_path};

    if (worker->download_file(url, temp_path)) {
        std::ifstream ifs(temp_path, std::ios::binary);
        if (ifs) {
            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            return content;
        }
    }
    
    return std::nullopt;
}

std::string get_sc_client_id(worker::WorkerClient* worker) {
    static std::string cached_id;
    static std::mutex cache_mu;
    std::scoped_lock lk{cache_mu};
    if (!cached_id.empty()) return cached_id;

    // use the worker helper
    auto html_opt = worker_http_get(worker, "https://soundcloud.com/discover");
    if (!html_opt) return "";
    std::string html = *html_opt;

    std::regex script_regex("src=\"(https://a-v2\\.sndcdn\\.com/assets/[^\"]+\\.js)\"");
    std::sregex_iterator words_begin(html.begin(), html.end(), script_regex);
    std::sregex_iterator words_end;
    std::string js_url;

    for (auto i = words_begin; i != words_end; ++i) {
        js_url = (*i)[1].str();
    }

    if (js_url.empty()) return "";

    // use the worker helper
    auto js_opt = worker_http_get(worker, js_url);
    if (!js_opt) return "";
    std::string js_content = *js_opt;
    
    std::regex client_id_regex("client_id\\s*:\\s*\"([a-zA-Z0-9]{32})\"");
    std::smatch match;

    if (std::regex_search(js_content, match, client_id_regex)) {
        cached_id = match[1].str();
    }
    return cached_id;
}

} // namespace

struct SoundCloudSource::Pipe {
    // -- worker mode: worker manages child processes --
    worker::WorkerClient* worker = nullptr;
    uint32_t pipeline_id = 0;

    // -- direct mode: DLL manages child processes (fallback) --
    HANDLE job        = nullptr;
    HANDLE proc_yt    = nullptr;
    HANDLE proc_ff    = nullptr;
    HANDLE proc_title = nullptr;

    // -- shared: data pipe handles (named pipe client or anonymous pipe read end) --
    HANDLE read_pipe  = nullptr;
    HANDLE title_pipe = nullptr;
    std::string title_buf;
    std::uint64_t bytes_written = 0;
    bool ended                  = false;

    // Identity + metadata travel with the pipeline so prefetch promotion
    // carries the (already-resolved) title without a "(loading)" flash.
    std::size_t for_queue_idx = 0;
    TrackInfo info{};

    ~Pipe() {
        if (read_pipe) { CloseHandle(read_pipe); read_pipe = nullptr; }
        if (title_pipe) { CloseHandle(title_pipe); title_pipe = nullptr; }
        // Worker mode: ask worker to terminate the child tree.
        if (worker && pipeline_id) worker->kill_pipeline(pipeline_id);
        // Direct mode: terminate each child tree (no-op in worker mode).
        subprocess::reap(proc_yt);
        subprocess::reap(proc_ff);
        subprocess::reap(proc_title);
        if (job) CloseHandle(job);
    }
};

SoundCloudSource::SoundCloudSource(SoundCloudConfig cfg, std::filesystem::path ffmpeg_path,
                                       worker::WorkerClient* worker)
    : cfg_{std::move(cfg)}, ffmpeg_path_{std::move(ffmpeg_path)}, worker_{worker} {}

SoundCloudSource::~SoundCloudSource() { 
    // bump generation so any running threads exit early
    queue_generation_.fetch_add(1, std::memory_order_release);
    
    if (hydrate_thread_.joinable()) {
        hydrate_thread_.join();
    }
    for (auto& t : old_hydrate_threads_) {
        if (t.joinable()) t.join();
    }

    {
        std::scoped_lock lk{mu_};
        discard_prefetch_locked();
        stop_pipe_locked();
    }
}

const SoundCloudStation* SoundCloudSource::active_station_locked() const noexcept {
    if (cfg_.stations.empty()) return nullptr;
    for (const auto& s : cfg_.stations)
        if (s.name == cfg_.active_station) return &s;
    return &cfg_.stations.front();
}

void SoundCloudSource::set_config(SoundCloudConfig cfg) {
    {
        std::scoped_lock lk{mu_};
        const auto* old_st = active_station_locked();
        std::string old_url = old_st ? old_st->url : "";

        cfg_ = std::move(cfg);
        
        const auto* new_st = active_station_locked();
        std::string new_url = new_st ? new_st->url : "";
        
        if (old_url != new_url && target_url_.empty()) {
            discard_prefetch_locked();
            stop_pipe_locked();
            queue_.clear();
            queue_idx_ = 0;
            queue_built_for_.clear();
        }
    }
}

void SoundCloudSource::set_active_station(std::string name) {
    std::scoped_lock lk{mu_};
    if (cfg_.active_station == name && target_url_.empty()) return;
    cfg_.active_station = std::move(name);
    target_url_.clear(); // clear temporary cast target
    discard_prefetch_locked();
    stop_pipe_locked();
    queue_.clear();
    queue_idx_ = 0;
    queue_built_for_.clear();
}

std::size_t SoundCloudSource::station_count() const noexcept {
    std::scoped_lock lk{mu_};
    return cfg_.stations.size();
}

std::string SoundCloudSource::active_station_name() const {
    std::scoped_lock lk{mu_};
    const SoundCloudStation* st = active_station_locked();
    return st ? st->name : std::string{};
}

bool SoundCloudSource::initialize() {
    if (!cfg_.enabled) return false;

    std::scoped_lock lk{mu_};
    if (cfg_.active_station.empty() && !cfg_.stations.empty()) {
        cfg_.active_station = cfg_.stations.front().name;
    }

    auth_ = cfg_.cookies_path.empty() ? AuthState::none_required : AuthState::authenticated;
    return true;
}

void SoundCloudSource::shutdown() noexcept {
    std::scoped_lock lk{mu_};
    discard_prefetch_locked();
    stop_pipe_locked();
}

void SoundCloudSource::set_target(std::string url) {
    std::scoped_lock lk{mu_};
    target_url_ = std::move(url);
    // Invalidate queue so the next play() re-resolves against the new URL.
    queue_.clear();
    queue_idx_ = 0;
    queue_built_for_.clear();
    discard_prefetch_locked();
}

void SoundCloudSource::set_ffmpeg_path(std::filesystem::path p) {
    std::scoped_lock lk{mu_};
    ffmpeg_path_ = std::move(p);
}

void SoundCloudSource::set_yt_dlp_path(std::filesystem::path p) {
    std::scoped_lock lk{mu_};
    yt_dlp_path_ = std::move(p);
}

void SoundCloudSource::set_shuffle(bool shuffle) {
    std::scoped_lock lk{mu_};
    cfg_.shuffle = shuffle;
    // The next-queue-index URL is about to change; any prefetched pipeline
    // would now be playing the wrong track.
    discard_prefetch_locked();

    std::string effective_url = target_url_;
    if (effective_url.empty()) {
        const auto* st = active_station_locked();
        if (st) effective_url = st->url;
    }

    if (!queue_.empty() && queue_built_for_ == effective_url) {
        auto current_track = queue_[queue_idx_];
        
        if (shuffle) {
            static thread_local std::mt19937 rng{std::random_device{}()};
            queue_.erase(queue_.begin() + queue_idx_);
            std::shuffle(queue_.begin(), queue_.end(), rng);
            queue_.insert(queue_.begin(), current_track);
            queue_idx_ = 0;
        } else {
            std::sort(queue_.begin(), queue_.end(), [](const auto& a, const auto& b) { 
                return a.original_index < b.original_index;
            });
            // find where the current track was stored
            for (std::size_t i = 0; i < queue_.size(); ++i) {
                if (queue_[i].original_index == current_track.original_index) {
                    queue_idx_ = i;
                    break;
                }
            }
        }
        // tell the UI that the queue has changed
        queue_version_.fetch_add(1, std::memory_order_release);
    }
}

SoundCloudSource::QueueSnapshot SoundCloudSource::queue_snapshot() const {
    std::scoped_lock lk{mu_};
    QueueSnapshot snap;
    snap.cursor = queue_idx_;
    snap.entries.reserve(queue_.size());
    for (std::size_t i = 0; i < queue_.size(); ++i) {
        snap.entries.push_back({i, queue_[i].url, queue_[i].title, queue_[i].artist});
    }
    return snap;
}

bool SoundCloudSource::jump_to(std::size_t index) {
    std::scoped_lock lk{mu_};
    if (index >= queue_.size()) return false;

    std::size_t start = index;
    while (queue_[index].title == "Unavailable / DRM") {
        index = (index + 1) % queue_.size();
        if (index == start) return false;
    }

    consecutive_failed_ = 0;
    queue_idx_ = index;
    if (!promote_prefetch_locked(queue_idx_)) start_pipe_locked();
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
    return true;
}

void SoundCloudSource::resolve_queue_locked() {
    std::string effective_url = target_url_;
    if (effective_url.empty()) {
        const auto* st = active_station_locked();
        if (st) effective_url = st->url;
    }

    if (effective_url.empty()) {
        queue_.clear();
        queue_idx_ = 0;
        queue_built_for_.clear();
        return;
    }

    if (queue_built_for_ == effective_url && !queue_.empty()) return;

    queue_.clear();
    queue_idx_ = 0;

    if (!is_playlist_url(effective_url)) {
        // add a 0 at the end for the original_index
        queue_.push_back({effective_url, "(loading)", "SoundCloud", 0}); 
        queue_version_.fetch_add(1, std::memory_order_release);
        queue_built_for_ = effective_url;
        const uint64_t gen = ++queue_generation_;
        if (hydrate_thread_.joinable()) {
            old_hydrate_threads_.push_back(std::move(hydrate_thread_));
        }
        hydrate_thread_ = std::thread{[this, gen]() { hydrate_queue(gen); }};
        return;
    }

    // Playlist URL: enumerate IDs via --flat-playlist.
    const auto yt  = yt_dlp_path_.empty() ? L"yt-dlp" : yt_dlp_path_.wstring();
    std::wstring cmd = quote(yt) + L" --ignore-config --no-warnings --flat-playlist --skip-download "
                                   L"--encoding UTF-8 "
                                   L"--print \"%(url)s\t%(title)s\t%(uploader)s\" ";
    if (!cfg_.cookies_path.empty())
        cmd += L"--cookies " + quote(cfg_.cookies_path.wstring()) + L" ";
    cmd += L"-- " + quote(widen(effective_url));

    // Prefer the worker (no fork of the game process); fall back to a direct
    // spawn when it's absent or returns nothing.
    std::string raw;
    if (worker_ && worker_->alive()) raw = worker_->run_capture(cmd);

    if (raw.empty()) {
        HANDLE job = create_kill_on_close_job();
        if (!job) {
            log::warn("[sc] resolve_queue: CreateJobObject failed ({})", GetLastError());
            return;
        }
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE rd = nullptr, wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) { CloseHandle(job); return; }
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul_in  = open_nul(GENERIC_READ);
        HANDLE err_log = open_stderr_log();
        HANDLE proc = spawn_in_job(job, cmd, nul_in, wr, err_log);
        const DWORD ec_yt = proc ? 0u : GetLastError();
        CloseHandle(wr);
        if (nul_in)  CloseHandle(nul_in);
        if (err_log) CloseHandle(err_log);
        if (!proc) {
            CloseHandle(rd); CloseHandle(job);
            log::warn("[sc] resolve_queue: failed to launch yt-dlp -- {}",
                      describe_launch_failure(std::wstring{yt}, ec_yt, !yt_dlp_path_.empty()));
            return;
        }
        raw = drain_to_eof(rd);
        CloseHandle(rd); CloseHandle(proc); CloseHandle(job);
    }
    std::size_t og_idx = 0;
    for (std::size_t pos = 0; pos < raw.size();) {
        auto nl   = raw.find('\n', pos);
        auto end  = (nl == std::string::npos) ? raw.size() : nl;
        auto line = raw.substr(pos, end - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
            
        if (!line.empty() && line != "NA") {
            auto tab1 = line.find('\t');
            std::string id = tab1 == std::string::npos ? line : line.substr(0, tab1);
            
            if (!id.empty() && id != "NA") {
                std::string title;
                std::string artist;
                
                // parse the 3 tab-separated values
                if (tab1 != std::string::npos) {
                    auto tab2 = line.find('\t', tab1 + 1);
                    title = line.substr(tab1 + 1, tab2 == std::string::npos ? std::string::npos : tab2 - (tab1 + 1));
                    if (tab2 != std::string::npos) artist = line.substr(tab2 + 1);
                }

                if (title.empty() || title == "NA") {
                    title = "(loading)";
                    auto com_pos = id.find("soundcloud.com/");
                    if (com_pos != std::string::npos && id.find("api-v2") == std::string::npos) {
                        std::string path = id.substr(com_pos + 15);
                        auto slash = path.find('/');
                        if (slash != std::string::npos) {
                            std::string ext_artist = path.substr(0, slash);
                            std::string ext_title = path.substr(slash + 1);
                            auto qm = ext_title.find('?');
                            if (qm != std::string::npos) ext_title = ext_title.substr(0, qm);
                            std::replace(ext_artist.begin(), ext_artist.end(), '-', ' ');
                            std::replace(ext_title.begin(), ext_title.end(), '-', ' ');
                            if (!ext_title.empty()) title = ext_title;
                            if (!ext_artist.empty() && (artist.empty() || artist == "NA")) artist = ext_artist;
                        }
                    }
                }
                
                if (artist.empty() || artist == "NA") artist = "SoundCloud";
                queue_.push_back({id, std::move(title), std::move(artist), og_idx++}); 
            }
        }
        pos = (nl == std::string::npos) ? raw.size() : nl + 1;
    }

    if (cfg_.shuffle && queue_.size() > 1) {
        static thread_local std::mt19937 rng{std::random_device{}()};
        std::shuffle(queue_.begin(), queue_.end(), rng);
    }

    queue_built_for_ = effective_url;
    queue_version_.fetch_add(1, std::memory_order_release);

    const uint64_t gen = ++queue_generation_;
    if (!queue_.empty()) {
        if (hydrate_thread_.joinable()) {
            old_hydrate_threads_.push_back(std::move(hydrate_thread_));
        }
        hydrate_thread_ = std::thread{[this, gen]() { hydrate_queue(gen); }};
    }

    if (queue_.empty() && is_playlist_url(effective_url)) {
        log::warn("[sc] resolved 0 tracks from {} -- check {} for yt-dlp errors "
                  "(private/deleted/geo-blocked playlist?)",
                  effective_url, stderr_log_path().string());
    } else {
        log::info("[sc] resolved {} track(s) from {}", queue_.size(), effective_url);
    }
}

std::unique_ptr<SoundCloudSource::Pipe>
SoundCloudSource::spawn_pipe_locked(std::string_view url, std::size_t for_idx) {
    const std::string play_url{url};

    auto pipe           = std::make_unique<Pipe>();
    pipe->for_queue_idx = for_idx;
    pipe->info.title    = "(loading)";
    pipe->info.artist   = "SoundCloud";

    const auto yt = yt_dlp_path_.empty() ? L"yt-dlp" : yt_dlp_path_.wstring();
    const auto ff = ffmpeg_path_.empty() ? L"ffmpeg" : ffmpeg_path_.wstring();

    std::wstring yt_cmd = quote(yt) + L" --ignore-config --no-warnings --no-progress "
                                      L"--no-write-thumbnail "
                                      L"--format bestaudio/best --no-playlist -o - ";
                                      
    // force yt-dlp to use ffmpeg for HLS streams
    if (!ffmpeg_path_.empty()) {
        yt_cmd += L"--ffmpeg-location " + quote(ffmpeg_path_.wstring()) + L" ";
        yt_cmd += L"--downloader ffmpeg ";
        yt_cmd += L"--downloader-args \"ffmpeg:-loglevel error\" ";
    }

    if (!cfg_.cookies_path.empty())
        yt_cmd += L"--cookies " + quote(cfg_.cookies_path.wstring()) + L" ";
    yt_cmd += L"-- " + quote(widen(play_url));

    std::wstring ff_cmd = quote(ff) + L" -loglevel error -i pipe:0 -f s16le ";
    if (volume_norm_.load(std::memory_order_acquire))
        ff_cmd += L"-af loudnorm=I=-14:TP=-2:LRA=11 ";
    ff_cmd += L"-acodec pcm_s16le -ar 48000 -ac 2 pipe:1";

    std::wstring tl_cmd = quote(yt) + L" --ignore-config --skip-download --no-warnings --no-playlist "
                                      L"--no-write-thumbnail "
                                      L"--encoding UTF-8 "
                                      L"--print \"%(title)s\" "
                                      L"--print \"%(uploader)s\" "
                                      L"--print \"%(duration)s\" "
                                      L"--print \"%(thumbnail)s\" ";
    if (!cfg_.cookies_path.empty())
        tl_cmd += L"--cookies " + quote(cfg_.cookies_path.wstring()) + L" ";
    tl_cmd += L"-- " + quote(widen(play_url));

    // Worker path: delegate every CreateProcess to the worker. Falls through to
    // the direct path below if the worker is absent or the spawn fails.
    if (worker_ && worker_->alive()) {
        if (auto result = worker_->spawn_pipeline({yt_cmd, ff_cmd}, tl_cmd); result.ok) {
            pipe->worker      = worker_;
            pipe->pipeline_id = result.pipeline_id;
            pipe->read_pipe   = result.pcm_pipe;
            pipe->title_pipe  = result.meta_pipe;
            log::info("[sc] pipe started via worker for {} (track {}/{})", play_url, for_idx + 1,
                      queue_.size());
            return pipe;
        }
        log::warn("[sc] worker spawn failed for {} -- falling back to direct spawn", play_url);
    }

    // Direct path: spawn the yt-dlp | ffmpeg chain + title resolver ourselves.
    pipe->job = create_kill_on_close_job();
    if (!pipe->job) {
        log::warn("[sc] CreateJobObject failed ({})", GetLastError());
        return nullptr;
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE yt_out_r = nullptr, yt_out_w = nullptr;
    HANDLE ff_out_r = nullptr, ff_out_w = nullptr;
    HANDLE tl_out_r = nullptr, tl_out_w = nullptr;

    auto bail = [&] {
        if (yt_out_r) CloseHandle(yt_out_r);
        if (yt_out_w) CloseHandle(yt_out_w);
        if (ff_out_r) CloseHandle(ff_out_r);
        if (ff_out_w) CloseHandle(ff_out_w);
        if (tl_out_r) CloseHandle(tl_out_r);
        if (tl_out_w) CloseHandle(tl_out_w);
    };

    if (!CreatePipe(&yt_out_r, &yt_out_w, &sa, 1 << 20)) {
        bail();
        return nullptr;
    }
    SetHandleInformation(yt_out_r, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    if (!CreatePipe(&ff_out_r, &ff_out_w, &sa, 1 << 20)) {
        bail();
        return nullptr;
    }
    SetHandleInformation(ff_out_r, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&tl_out_r, &tl_out_w, &sa, 1 << 16)) {
        bail();
        return nullptr;
    }
    SetHandleInformation(tl_out_r, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul_in  = open_nul(GENERIC_READ);
    HANDLE err_log = open_stderr_log();

    pipe->proc_yt = spawn_in_job(pipe->job, yt_cmd, nul_in, yt_out_w, err_log);
    const DWORD ec_yt = pipe->proc_yt ? 0u : GetLastError();
    CloseHandle(yt_out_w); yt_out_w = nullptr;
    if (!pipe->proc_yt) {
        log::warn("[sc] failed to launch yt-dlp -- {}",
                  describe_launch_failure(std::wstring{yt}, ec_yt, !yt_dlp_path_.empty()));
        bail();
        if (nul_in) CloseHandle(nul_in);
        if (err_log) CloseHandle(err_log);
        return nullptr;
    }

    pipe->proc_ff     = spawn_in_job(pipe->job, ff_cmd, yt_out_r, ff_out_w, err_log);
    const DWORD ec_ff = pipe->proc_ff ? 0u : GetLastError();
    CloseHandle(yt_out_r);
    yt_out_r = nullptr;
    CloseHandle(ff_out_w);
    ff_out_w = nullptr;
    if (!pipe->proc_ff) {
        log::warn("[sc] failed to launch ffmpeg -- {}",
                  describe_launch_failure(std::wstring{ff}, ec_ff, !ffmpeg_path_.empty()));
        if (ff_out_r) CloseHandle(ff_out_r);
        if (tl_out_r) CloseHandle(tl_out_r);
        if (tl_out_w) CloseHandle(tl_out_w);
        if (nul_in)   CloseHandle(nul_in);
        if (err_log)  CloseHandle(err_log);
        return nullptr;
    }

    pipe->proc_title = spawn_in_job(pipe->job, tl_cmd, nul_in, tl_out_w, err_log);
    CloseHandle(tl_out_w); tl_out_w = nullptr;
    if (pipe->proc_title) {
        pipe->title_pipe = tl_out_r; tl_out_r = nullptr;
    } else if (tl_out_r) {
        CloseHandle(tl_out_r); tl_out_r = nullptr;
    }

    if (nul_in) CloseHandle(nul_in);
    if (err_log) CloseHandle(err_log);

    pipe->read_pipe = ff_out_r;

    log::info("[sc] pipe started for {} (track {}/{}; child stderr -> {})", play_url, for_idx + 1,
              queue_.size(), stderr_log_path().string());
    return pipe;
}

void SoundCloudSource::start_pipe_locked() {
    stop_pipe_locked();
    resolve_queue_locked();
    if (queue_.empty()) return;
    if (queue_idx_ >= queue_.size()) queue_idx_ = 0;

    std::size_t start = queue_idx_;
    while (queue_[queue_idx_].title == "Unavailable / DRM") {
        queue_idx_ = (queue_idx_ + 1) % queue_.size();
        if (queue_idx_ == start) break;
    }

    pipe_ = spawn_pipe_locked(queue_[queue_idx_].url, queue_idx_);
    if (!pipe_) return;
    position_ms_.store(0, std::memory_order_release);
    state_.store(PlaybackState::buffering, std::memory_order_release);
}

std::size_t SoundCloudSource::next_queue_idx_locked() const noexcept {
    if (queue_.empty()) return 0;
    std::size_t next_idx = (queue_idx_ + 1) % queue_.size();

    std::size_t start = next_idx;
    while (queue_[next_idx].title == "Unavailable / DRM") {
        next_idx = (next_idx + 1) % queue_.size();
        if (next_idx == start) break;
    }
    return next_idx;
}

void SoundCloudSource::discard_prefetch_locked() noexcept {
    // ~Pipe closes the job; KILL_ON_JOB_CLOSE reaps yt-dlp+ffmpeg+title.
    prefetch_.reset();
}

bool SoundCloudSource::promote_prefetch_locked(std::size_t expected_idx) {
    if (!prefetch_ || prefetch_->for_queue_idx != expected_idx) {
        discard_prefetch_locked();
        return false;
    }
    pipe_ = std::move(prefetch_);
    // Ring still drains leftover PCM from the previous track; the new pipe's
    // OS pipe buffer (~1 MB) already holds ~5 s of decoded audio, so PCM
    // continuity is preserved and the "(loading)" label never appears.
    position_ms_.store(0, std::memory_order_release);
    state_.store(PlaybackState::buffering, std::memory_order_release);
    return true;
}

void SoundCloudSource::maybe_spawn_prefetch_locked() {
    if (!prebuffer_next_.load(std::memory_order_acquire)) return;
    if (prefetch_ || !pipe_ || queue_.size() < 2) return;
    // Only commit a second yt-dlp once the current pipe has proven viable
    // (~0.5 s of PCM = 96 KB). Saves spawning a prefetch we'll throw away if
    // the current track turns out to be unfetchable.
    constexpr std::uint64_t kViableBytes = 96 * 1024;
    if (pipe_->bytes_written < kViableBytes) return;

    const std::size_t idx = next_queue_idx_locked();
    prefetch_             = spawn_pipe_locked(queue_[idx].url, idx);
}

void SoundCloudSource::stop_pipe_locked() {
    // Note: prefetch_ is intentionally NOT touched here -- start_pipe_locked()
    // calls stop_pipe_locked() before promotion, and we'd lose the prefetched
    // pipeline. Callers that want a clean shutdown call discard_prefetch_locked()
    // explicitly (stop(), shutdown()).
    pipe_.reset();
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

void SoundCloudSource::play() {
    std::scoped_lock lk{mu_};
    if (!pipe_) start_pipe_locked();
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
}

void SoundCloudSource::pause() { state_.store(PlaybackState::paused, std::memory_order_release); }

bool SoundCloudSource::restart_current() {
    std::scoped_lock lk{mu_};
    if (queue_.empty()) return false;
    consecutive_failed_ = 0;
    start_pipe_locked(); // re-pipe from t=0 at the same queue_idx_; prefetch is still valid
    if (!pipe_) return false;
    state_.store(PlaybackState::playing, std::memory_order_release);
    return true;
}

bool SoundCloudSource::skip_next() {
    std::scoped_lock lk{mu_};
    if (queue_.empty()) return false;
    consecutive_failed_ = 0;
    queue_idx_          = next_queue_idx_locked();
    if (!promote_prefetch_locked(queue_idx_)) start_pipe_locked();
    if (!pipe_) return false;
    state_.store(PlaybackState::playing, std::memory_order_release);
    return true;
}

void SoundCloudSource::stop() {
    std::scoped_lock lk{mu_};
    discard_prefetch_locked();
    stop_pipe_locked();
    target_url_.clear(); // allow falling back to active station next play
}

void SoundCloudSource::next() {
    std::scoped_lock lk{mu_};
    if (queue_.empty()) return;
    consecutive_failed_ = 0; // user override clears the give-up state
    queue_idx_          = next_queue_idx_locked();
    if (!promote_prefetch_locked(queue_idx_)) start_pipe_locked();
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
}

void SoundCloudSource::previous() {
    std::scoped_lock lk{mu_};
    if (queue_.empty()) return;
    consecutive_failed_ = 0;
    const auto n        = static_cast<std::ptrdiff_t>(queue_.size());
    auto i              = static_cast<std::ptrdiff_t>(queue_idx_) - 1;
    std::size_t target  = static_cast<std::size_t>(((i % n) + n) % n);

    std::size_t start = target;
    while (queue_[target].title == "Unavailable / DRM") {
        i--;
        target = static_cast<std::size_t>(((i % n) + n) % n);
        if (target == start) break;
    }
    queue_idx_ = target;

    // Prefetch targets idx+1; previous() rewinds, so it's stale.
    discard_prefetch_locked();
    start_pipe_locked();
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
}

TrackInfo SoundCloudSource::current_track() const {
    std::scoped_lock lk{mu_};
    TrackInfo t;
    if (pipe_) t = pipe_->info;
    t.position_ms = position_ms_.load(std::memory_order_acquire);
    return t;
}

void SoundCloudSource::set_playback_options(const PlaybackConfig& opts) {
    // 48 kHz matches the ffmpeg pipe.
    eq_.set_options(opts.equalizer_enabled, opts.equalizer_bands, 48000.0f);
    // loudnorm is in the ffmpeg argv; the new state is picked up on the next
    // start_pipe_locked() (track change). Same per-track granularity as
    // local-files ReplayGain -- not re-fetching the current track.
    volume_norm_.store(opts.volume_normalization, std::memory_order_release);
    // Toggling prebuffer off mid-playback drops any in-flight prefetch
    // process so we don't hold an orphan yt-dlp for the whole track.
    const bool prev =
        prebuffer_next_.exchange(opts.prebuffer_next_track, std::memory_order_acq_rel);
    if (prev && !opts.prebuffer_next_track) {
        std::scoped_lock lk{mu_};
        discard_prefetch_locked();
    }
}

std::string SoundCloudSource::auth_instructions() const {
    return "Export your SoundCloud cookies to a Netscape cookies.txt and set "
           "[soundcloud].cookies_path in config.toml. Public content works "
           "without cookies.";
}

void SoundCloudSource::drain_title_pipe_locked(Pipe* p) {
    // yt-dlp --print closes its stdout last; the Peek right after may go
    // ERROR_BROKEN_PIPE before we've drained the buffered "title\nuploader\n
    // duration\n", so we finalise on broken-pipe too.
    if (!p || !p->title_pipe) return;

    bool finalise = false;
    for (int safety = 0; safety < 8; ++safety) {
        DWORD tavail = 0;
        BOOL ok      = PeekNamedPipe(p->title_pipe, nullptr, 0, nullptr, &tavail, nullptr);
        if (!ok) {
            finalise = true;
            break;
        }
        if (tavail == 0) {
            DWORD ec = STILL_ACTIVE;
            if (p->proc_title && GetExitCodeProcess(p->proc_title, &ec) && ec != STILL_ACTIVE)
                finalise = true;
            break;
        }
        char tbuf[1024];
        DWORD got = 0;
        if (!ReadFile(p->title_pipe, tbuf, sizeof(tbuf), &got, nullptr) || got == 0) {
            finalise = true;
            break;
        }
        p->title_buf.append(tbuf, got);
    }
    if (!finalise) return;

    auto& s        = p->title_buf;
    auto take_line = [&] {
        auto nl          = s.find('\n');
        std::string line = (nl == std::string::npos) ? s : s.substr(0, nl);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        s.erase(0, nl == std::string::npos ? s.size() : nl + 1);
        return line;
    };
    auto title    = take_line();
    auto uploader = take_line();
    auto duration = take_line();
    auto thumb    = take_line();
    if (!title.empty() && title != "NA") p->info.title = std::move(title);
    if (!uploader.empty() && uploader != "NA") p->info.artist = std::move(uploader);
    try {
        if (!duration.empty() && duration != "NA")
            p->info.duration_ms = static_cast<std::uint64_t>(std::stod(duration) * 1000.0);
    } catch (...) {}
    // yt-dlp's %(thumbnail)s is a public URL the browser loads directly.
    if (!thumb.empty() && thumb != "NA") p->info.artwork_url = std::move(thumb);
    CloseHandle(p->title_pipe);
    p->title_pipe = nullptr;
}

void SoundCloudSource::pump(RingBuffer& ring) {
    {
        auto st = state_.load(std::memory_order_acquire);
        if (st != PlaybackState::playing && st != PlaybackState::buffering) return;
    }

    std::scoped_lock lk{mu_};
    Pipe* p = pipe_.get();
    if (!p) return;

    // Resolve titles on both the current pipe and the (silently buffering)
    // prefetch so promotion picks up an already-resolved TrackInfo.
    drain_title_pipe_locked(p);
    drain_title_pipe_locked(prefetch_.get());

    // ---- PCM drain ----
    auto advance_to_next = [&] {
        if (queue_.empty()) {
            stop_pipe_locked();
            return;
        }
        queue_idx_ = next_queue_idx_locked();
        if (!promote_prefetch_locked(queue_idx_)) start_pipe_locked();
        if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
    };

    auto update_position = [&] {
        const std::size_t r        = ring.readable();
        const std::uint64_t played = p->bytes_written > r ? p->bytes_written - r : 0;
        position_ms_.store(played * 1000ull / kPcmBytesPerSec, std::memory_order_release);
    };

    if (p->ended) {
        update_position();
        if (ring.readable() == 0) advance_to_next();
        return;
    }

    if (!p->read_pipe) return;

    auto on_eof = [&] {
        if (p->bytes_written == 0) {
            // a closed pipe with 0 bytes usually indicates a DRM block or region block
            // mark the entry as unavailable to skip it on all future passes
            if (p->for_queue_idx < queue_.size()) {
                queue_[p->for_queue_idx].title = "Unavailable / DRM";
                queue_[p->for_queue_idx].artist = "SoundCloud";
                queue_version_.fetch_add(1, std::memory_order_release);
            }

            if (++consecutive_failed_ >= 3) {
                log::warn("[sc] giving up after {} consecutive empty tracks", consecutive_failed_);
                stop_pipe_locked();
                return;
            }
            advance_to_next();
            return;
        }

        consecutive_failed_ = 0;
        p->ended            = true;
        if (p->read_pipe) {
            CloseHandle(p->read_pipe);
            p->read_pipe = nullptr;
        }
    };

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
        // Force whole stereo S16 frames so the EQ never sees half a sample;
        // the next iteration picks up the byte(s) we left in the pipe.
        want &= ~std::size_t{3};
        if (!want) break;
        std::byte buf[4096];
        DWORD got = 0;
        if (!ReadFile(p->read_pipe, buf, (DWORD)want, &got, nullptr) || got == 0) {
            on_eof();
            return;
        }
        const DWORD aligned = (got / 4u) * 4u;
        if (aligned) eq_.process(reinterpret_cast<int16_t*>(buf), aligned / 4u);
        ring.write(buf, aligned);
        p->bytes_written += aligned;
        update_position();
        avail = avail > got ? avail - got : 0;
        if (state_.load(std::memory_order_acquire) == PlaybackState::buffering &&
            ring.readable() > 32 * 1024)
            state_.store(PlaybackState::playing, std::memory_order_release);
    }
    // Even when the read loop didn't run (e.g. ring was full), keep position
    // moving as the mixer drains the ring.
    update_position();
    // Spawn the next-track pipeline once the current pipe has proven viable;
    // its OS pipe buffer fills silently in the background (~5 s of PCM) and is
    // promoted on the next transition.
    maybe_spawn_prefetch_locked();
}

void SoundCloudSource::hydrate_queue(uint64_t generation) {
    struct Unresolved {
        std::string original_url;
        std::string numeric_id;
    };
    std::vector<Unresolved> pending;

    {
        std::scoped_lock lk{mu_};
        if (queue_generation_.load(std::memory_order_acquire) != generation) return;
        
        for (std::size_t i = 0; i < queue_.size(); ++i) {
            if (queue_[i].title == "SoundCloud Track" || queue_[i].title == "(loading)" || queue_[i].title.empty()) {
                std::string num_id = queue_[i].url;
                auto tracks_pos = num_id.find("/tracks/");
                if (tracks_pos != std::string::npos) {
                    num_id = num_id.substr(tracks_pos + 8);
                    auto qm = num_id.find('?');
                    if (qm != std::string::npos) num_id = num_id.substr(0, qm);
                }
                pending.push_back({queue_[i].url, num_id});
            }
        }
    }

    if (pending.empty()) return;

    // ensure there's a valid client_id for the api
    std::string client_id = get_sc_client_id(worker_);
    if (client_id.empty()) {
        log::warn("[sc] failed to extract client_id for native hydration");
        return;
    }

    // unescape helper for json strings
    auto unescape = [](std::string s) {
        std::string r;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size() && (s[i+1] == '"' || s[i+1] == '\\')) {
                r += s[++i];
            } else r += s[i];
        }
        return r;
    };

    const std::size_t batch_size = 50;
    bool updated_any = false;

    for (std::size_t i = 0; i < pending.size(); i += batch_size) {
        if (queue_generation_.load(std::memory_order_acquire) != generation) return;

        std::string ids_csv;
        std::size_t chunk_end = std::min(i + batch_size, pending.size());
        
        for (std::size_t j = i; j < chunk_end; ++j) {
            ids_csv += pending[j].numeric_id;
            if (j < chunk_end - 1) ids_csv += "%2C"; // url encoded comma
        }

        std::string api_url = "https://api-v2.soundcloud.com/tracks?ids=" + ids_csv + "&client_id=" + client_id;

        // handle optional return from http_get
        auto raw_opt = worker_http_get(worker_, api_url);
        if (!raw_opt) continue;
        std::string raw = *raw_opt;

        std::scoped_lock lk{mu_};
        if (queue_generation_.load(std::memory_order_acquire) != generation) return;

        for (std::size_t j = i; j < chunk_end; ++j) {
            const auto& track = pending[j];
            
            // locate the track in the response array using the numeric ID
            std::string id_marker = "\"id\":" + track.numeric_id;
            auto pos = raw.find(id_marker);
            if (pos == std::string::npos) continue;

            // grab title
            std::string title = "(Unavailable)";
            auto title_pos = raw.find("\"title\":\"", pos);
            if (title_pos != std::string::npos) {
                title_pos += 9;
                auto title_end = raw.find("\"", title_pos);
                if (title_end != std::string::npos) {
                    title = unescape(raw.substr(title_pos, title_end - title_pos));
                }
            }

            // grab artist (username)
            std::string artist = "";
            auto user_pos = raw.find("\"user\":{", pos);
            if (user_pos != std::string::npos) {
                auto username_pos = raw.find("\"username\":\"", user_pos);
                if (username_pos != std::string::npos && username_pos < raw.find("}", user_pos)) {
                    username_pos += 12;
                    auto username_end = raw.find("\"", username_pos);
                    if (username_end != std::string::npos) {
                        artist = unescape(raw.substr(username_pos, username_end - username_pos));
                    }
                }
            }

            // apply parsed metadata directly to the queue entry
            auto it = std::find_if(queue_.begin(), queue_.end(), [&](const auto& entry) {
                return entry.url == track.original_url || entry.url.find(track.original_url) != std::string::npos;
            });
            
            if (it != queue_.end()) {
                it->title = std::move(title);
                it->artist = std::move(artist);
                updated_any = true;
            }
        }
        
        if (updated_any) {
            // tell the UI that items have been updated
            queue_version_.fetch_add(1, std::memory_order_release);
            updated_any = false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // rename queue entries that are missing metadata
    if (queue_generation_.load(std::memory_order_acquire) == generation) {
        std::scoped_lock lk{mu_};
        // ensure a playlist swap hasn't occurred before updating
        if (queue_generation_.load(std::memory_order_acquire) != generation) return;

        bool cleaned_up = false;
        for (auto& entry : queue_) {
            if (entry.title == "(loading)" || entry.title == "SoundCloud Track" || entry.title.empty()) {
                entry.title = "Unavailable / DRM";
                entry.artist = "SoundCloud";
                cleaned_up = true;
            }
        }

        if (cleaned_up) {
            queue_version_.fetch_add(1, std::memory_order_release);
        }
    }
}

} // namespace fh6::sources