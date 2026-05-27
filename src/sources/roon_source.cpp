#include "fh6/sources/roon_source.hpp"
#include "fh6/log.hpp"

#include <algorithm>
#include <array>
#include <mutex>
#include <utility>

namespace fh6::sources {
namespace {

class WasapiRoonCapture final : public IRoonCapture {
public:
    bool start(const audio::WasapiLoopbackCaptureConfig& cfg) override {
        return capture_.start(cfg);
    }
    void stop() noexcept override { capture_.stop(); }
    void clear() noexcept override { capture_.clear(); }
    audio::WasapiLoopbackCaptureStatus status() const override { return capture_.status(); }
    std::size_t read_pcm(void* dst, std::size_t bytes) noexcept override {
        return capture_.read_pcm(dst, bytes);
    }

private:
    audio::WasapiLoopbackCapture capture_;
};

std::unique_ptr<IRoonCapture> make_default_capture() {
    return std::make_unique<WasapiRoonCapture>();
}

} // namespace

RoonSource::RoonSource(RoonConfig cfg, std::filesystem::path data_dir,
                       RoonCaptureFactory capture_factory)
    : cfg_{std::move(cfg)}, data_dir_{std::move(data_dir)},
      capture_factory_{std::move(capture_factory)} {
    if (!capture_factory_) capture_factory_ = make_default_capture;
}

RoonSource::~RoonSource() { shutdown(); }

bool RoonSource::initialize() {
    if (!cfg_.enabled) {
        log::info("[roon] initialize skipped: source disabled");
        return false;
    }
    if (cfg_.auto_start_bridge) {
        sidecar_ = std::make_unique<roon::RoonSidecarProcess>(cfg_, data_dir_);
        if (!sidecar_->start()) {
            set_setup_error(sidecar_->error());
        }
    }
    initialized_.store(true, std::memory_order_release);
    state_.store(PlaybackState::stopped, std::memory_order_release);
    log::info(
        "[roon] source initialized; auto_start_bridge={} zone_selected={} capture_selected={}",
        cfg_.auto_start_bridge, !cfg_.selected_zone_id.empty(), !cfg_.capture_device_id.empty());
    return true;
}

void RoonSource::shutdown() noexcept {
    stop_capture();
    if (sidecar_) sidecar_->stop();
    state_.store(PlaybackState::stopped, std::memory_order_release);
    initialized_.store(false, std::memory_order_release);
    log::info("[roon] source shutdown");
}

void RoonSource::play() {
    log::info("[roon] play requested");
    if (!initialized_.load(std::memory_order_acquire)) return;
    if (!start_capture()) {
        state_.store(PlaybackState::stopped, std::memory_order_release);
        return;
    }
    state_.store(PlaybackState::playing, std::memory_order_release);
}

void RoonSource::pause() {
    log::info("[roon] pause requested");
    if (!initialized_.load(std::memory_order_acquire)) return;
    stop_capture();
    state_.store(PlaybackState::paused, std::memory_order_release);
}

void RoonSource::stop() {
    log::info("[roon] stop requested");
    stop_capture();
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

void RoonSource::next() {
    log::info("[roon] next requested");
    clear_capture();
    play();
}

void RoonSource::previous() {
    log::info("[roon] previous requested");
    clear_capture();
    play();
}

void RoonSource::pump(RingBuffer& ring) {
    if (state_.load(std::memory_order_acquire) != PlaybackState::playing || !capture_) return;

    std::array<std::byte, 8192> scratch{};
    while (ring.writable() >= 4) {
        auto request  = std::min(scratch.size(), ring.writable());
        request      -= request % 4;
        if (request == 0) return;
        const auto got = capture_->read_pcm(scratch.data(), request);
        if (got == 0) return;
        if (ring.write(scratch.data(), got) != got) return;
    }
}

TrackInfo RoonSource::current_track() const { return info_; }

PlaybackState RoonSource::playback_state() const noexcept {
    return state_.load(std::memory_order_acquire);
}

AuthState RoonSource::auth_state() const noexcept { return setup_state(); }

std::string RoonSource::auth_instructions() const {
    if (auto err = setup_error(); !err.empty()) return err;
    if (cfg_.bridge_path.empty()) {
        return "Set [roon].bridge_path to tools\\roon-bridge\\index.mjs or another Roon sidecar "
               "script path.";
    }
    if (cfg_.selected_zone_id.empty() && cfg_.capture_device_id.empty()) {
        return "Authorize FH6 Universal Radio in Roon, then select a Roon zone and Windows "
               "capture device in Web Control.";
    }
    if (cfg_.selected_zone_id.empty()) {
        return "Select a Roon zone in Web Control.";
    }
    if (cfg_.capture_device_id.empty()) {
        return "Select a Windows capture device that receives the selected Roon zone audio.";
    }
    return {};
}

AuthState RoonSource::setup_state() const noexcept {
    if (setup_error_present_.load(std::memory_order_acquire)) return AuthState::error;
    if (cfg_.bridge_path.empty()) return AuthState::error;
    if (cfg_.selected_zone_id.empty() || cfg_.capture_device_id.empty())
        return AuthState::needs_auth;
    return AuthState::authenticated;
}

std::string RoonSource::setup_error() const {
    std::scoped_lock lk{setup_mu_};
    return setup_error_;
}

void RoonSource::set_setup_error(std::string message) {
    std::scoped_lock lk{setup_mu_};
    log::warn("[roon] setup error: {}", message);
    setup_error_ = std::move(message);
    setup_error_present_.store(true, std::memory_order_release);
}

bool RoonSource::start_capture() {
    if (cfg_.capture_device_id.empty()) {
        log::warn("[roon] capture start blocked: no capture device selected");
        return false;
    }
    if (!capture_) capture_ = capture_factory_();
    if (!capture_) {
        set_setup_error("Roon WASAPI capture could not be created.");
        return false;
    }

    audio::WasapiLoopbackCaptureConfig cfg;
    cfg.device_id  = cfg_.capture_device_id;
    cfg.latency_ms = cfg_.latency_ms;
    cfg.queue_ms   = std::clamp<uint32_t>(cfg_.latency_ms * 4U, 250U, 5000U);
    log::info("[roon] starting capture device_id={} latency_ms={} queue_ms={}", cfg.device_id,
              cfg.latency_ms, cfg.queue_ms);
    if (capture_->start(cfg)) {
        log::info("[roon] capture started");
        return true;
    }

    auto status = capture_->status();
    log::warn("[roon] capture start failed: {}",
              status.error.empty() ? "unknown error" : status.error);
    set_setup_error(status.error.empty() ? "Roon WASAPI capture failed to start." : status.error);
    return false;
}

void RoonSource::stop_capture() noexcept {
    if (!capture_) return;
    log::info("[roon] stopping capture");
    capture_->stop();
    capture_->clear();
}

void RoonSource::clear_capture() noexcept {
    if (capture_) {
        log::info("[roon] clearing capture queue");
        capture_->clear();
    }
}

} // namespace fh6::sources
