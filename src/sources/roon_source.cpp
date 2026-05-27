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

class RoonClientControl final : public IRoonControl {
public:
    roon::RoonCommandResult transport(std::string_view control, std::string_view zone_id) override {
        return client_.transport(control, zone_id);
    }

private:
    roon::RoonControlClient client_;
};

std::unique_ptr<IRoonCapture> make_default_capture() {
    return std::make_unique<WasapiRoonCapture>();
}

std::unique_ptr<IRoonControl> make_default_control() {
    return std::make_unique<RoonClientControl>();
}

} // namespace

RoonSource::RoonSource(RoonConfig cfg, std::filesystem::path data_dir,
                       RoonCaptureFactory capture_factory, RoonControlFactory control_factory)
    : cfg_{std::move(cfg)}, data_dir_{std::move(data_dir)},
      capture_factory_{std::move(capture_factory)}, control_factory_{std::move(control_factory)} {
    if (!capture_factory_) capture_factory_ = make_default_capture;
    if (!control_factory_) control_factory_ = make_default_control;
}

RoonSource::~RoonSource() { shutdown(); }

bool RoonSource::initialize() {
    const auto cfg = config_snapshot();
    if (!cfg.enabled) {
        log::info("[roon] initialize skipped: source disabled");
        return false;
    }
    if (cfg.auto_start_bridge) {
        sidecar_ = std::make_unique<roon::RoonSidecarProcess>(cfg, data_dir_);
        if (!sidecar_->start()) {
            set_setup_error(sidecar_->error());
        }
    }
    initialized_.store(true, std::memory_order_release);
    state_.store(PlaybackState::stopped, std::memory_order_release);
    log::info(
        "[roon] source initialized; auto_start_bridge={} zone_selected={} capture_selected={}",
        cfg.auto_start_bridge, !cfg.selected_zone_id.empty(), !cfg.capture_device_id.empty());
    return true;
}

void RoonSource::shutdown() noexcept {
    stop_capture();
    if (sidecar_) sidecar_->stop();
    state_.store(PlaybackState::stopped, std::memory_order_release);
    initialized_.store(false, std::memory_order_release);
    log::info("[roon] source shutdown");
}

void RoonSource::update_config(RoonConfig cfg, std::filesystem::path data_dir) {
    RoonConfig old;
    std::filesystem::path old_dir;
    {
        std::scoped_lock lk{cfg_mu_};
        old     = cfg_;
        old_dir = data_dir_;
        cfg_    = std::move(cfg);
        if (!data_dir.empty()) data_dir_ = std::move(data_dir);
    }

    const auto next = config_snapshot();
    clear_setup_error();
    log::info("[roon] config updated; zone_selected={} capture_selected={}",
              !next.selected_zone_id.empty(), !next.capture_device_id.empty());

    const bool was_playing = state_.load(std::memory_order_acquire) == PlaybackState::playing;
    stop_capture();

    const bool sidecar_args_changed = old.auto_start_bridge != next.auto_start_bridge ||
                                      old.node_path != next.node_path ||
                                      old.bridge_path != next.bridge_path || old_dir != data_dir_;
    if (sidecar_args_changed && sidecar_) {
        sidecar_->stop();
        sidecar_.reset();
    }
    if (next.auto_start_bridge && initialized_.load(std::memory_order_acquire) &&
        (!sidecar_ || !sidecar_->running())) {
        sidecar_ = std::make_unique<roon::RoonSidecarProcess>(next, data_dir_);
        if (!sidecar_->start()) set_setup_error(sidecar_->error());
    } else if (!next.auto_start_bridge && sidecar_) {
        sidecar_->stop();
        sidecar_.reset();
    }

    if (was_playing) {
        if (start_capture()) {
            state_.store(PlaybackState::playing, std::memory_order_release);
        } else {
            state_.store(PlaybackState::stopped, std::memory_order_release);
        }
    }
}

void RoonSource::play() {
    log::info("[roon] play requested");
    if (!initialized_.load(std::memory_order_acquire)) return;
    if (!send_transport("play")) {
        state_.store(PlaybackState::stopped, std::memory_order_release);
        return;
    }
    if (!start_capture()) {
        state_.store(PlaybackState::stopped, std::memory_order_release);
        return;
    }
    state_.store(PlaybackState::playing, std::memory_order_release);
}

void RoonSource::pause() {
    log::info("[roon] pause requested");
    if (!initialized_.load(std::memory_order_acquire)) return;
    (void)send_transport("pause");
    stop_capture();
    state_.store(PlaybackState::paused, std::memory_order_release);
}

void RoonSource::stop() {
    log::info("[roon] stop requested");
    if (initialized_.load(std::memory_order_acquire)) (void)send_transport("stop");
    stop_capture();
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

void RoonSource::next() {
    log::info("[roon] next requested");
    if (!initialized_.load(std::memory_order_acquire)) return;
    (void)send_transport("next");
    clear_capture();
    if (state_.load(std::memory_order_acquire) == PlaybackState::playing) (void)start_capture();
}

void RoonSource::previous() {
    log::info("[roon] previous requested");
    if (!initialized_.load(std::memory_order_acquire)) return;
    (void)send_transport("previous");
    clear_capture();
    if (state_.load(std::memory_order_acquire) == PlaybackState::playing) (void)start_capture();
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
    const auto cfg = config_snapshot();
    if (auto err = setup_error(); !err.empty()) return err;
    if (cfg.bridge_path.empty()) {
        return "Set [roon].bridge_path to tools\\roon-bridge\\index.mjs or another Roon sidecar "
               "script path.";
    }
    if (cfg.selected_zone_id.empty() && cfg.capture_device_id.empty()) {
        return "Authorize FH6 Universal Radio in Roon, then select a Roon zone and Windows "
               "capture device in Web Control.";
    }
    if (cfg.selected_zone_id.empty()) {
        return "Select a Roon zone in Web Control.";
    }
    if (cfg.capture_device_id.empty()) {
        return "Select a Windows capture device that receives the selected Roon zone audio.";
    }
    return {};
}

RoonConfig RoonSource::config_snapshot() const {
    std::scoped_lock lk{cfg_mu_};
    return cfg_;
}

AuthState RoonSource::setup_state() const noexcept {
    const auto cfg = config_snapshot();
    if (setup_error_present_.load(std::memory_order_acquire)) return AuthState::error;
    if (cfg.bridge_path.empty()) return AuthState::error;
    if (cfg.selected_zone_id.empty() || cfg.capture_device_id.empty()) return AuthState::needs_auth;
    return AuthState::authenticated;
}

std::string RoonSource::setup_error() const {
    std::scoped_lock lk{setup_mu_};
    return setup_error_;
}

void RoonSource::clear_setup_error() {
    std::scoped_lock lk{setup_mu_};
    setup_error_.clear();
    setup_error_present_.store(false, std::memory_order_release);
}

void RoonSource::set_setup_error(std::string message) {
    std::scoped_lock lk{setup_mu_};
    log::warn("[roon] setup error: {}", message);
    setup_error_ = std::move(message);
    setup_error_present_.store(true, std::memory_order_release);
}

bool RoonSource::send_transport(std::string_view control) {
    const auto current = config_snapshot();
    if (current.selected_zone_id.empty()) {
        log::warn("[roon] transport {} blocked: no zone selected", control);
        return false;
    }
    if (!control_) control_ = control_factory_();
    if (!control_) {
        set_setup_error("Roon control client could not be created.");
        return false;
    }

    auto result = control_->transport(control, current.selected_zone_id);
    if (result.ok) return true;

    auto error = result.error.empty() ? "Roon transport command failed." : result.error;
    log::warn("[roon] transport {} failed status={}: {}", control, result.http_status, error);
    set_setup_error(std::move(error));
    return false;
}

bool RoonSource::start_capture() {
    const auto current = config_snapshot();
    if (current.capture_device_id.empty()) {
        log::warn("[roon] capture start blocked: no capture device selected");
        return false;
    }
    if (!capture_) capture_ = capture_factory_();
    if (!capture_) {
        set_setup_error("Roon WASAPI capture could not be created.");
        return false;
    }

    audio::WasapiLoopbackCaptureConfig cfg;
    cfg.device_id  = current.capture_device_id;
    cfg.latency_ms = current.latency_ms;
    cfg.queue_ms   = std::clamp<uint32_t>(current.latency_ms * 4U, 250U, 5000U);
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
