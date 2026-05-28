#pragma once

#include "fh6/audio_source.hpp"
#include "fh6/audio/wasapi_loopback_capture.hpp"
#include "fh6/config.hpp"
#include "fh6/roon/roon_control_client.hpp"
#include "fh6/roon/roon_sidecar_process.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace fh6::sources {

class IRoonCapture {
public:
    virtual ~IRoonCapture() = default;

    virtual bool start(const audio::WasapiLoopbackCaptureConfig& cfg)   = 0;
    virtual void stop() noexcept                                        = 0;
    virtual void clear() noexcept                                       = 0;
    virtual audio::WasapiLoopbackCaptureStatus status() const           = 0;
    virtual std::size_t read_pcm(void* dst, std::size_t bytes) noexcept = 0;
};

class IRoonControl {
public:
    virtual ~IRoonControl() = default;

    virtual roon::RoonCommandResult transport(std::string_view control,
                                              std::string_view zone_id) = 0;
    virtual roon::RoonStatus status()                                   = 0;
};

using RoonCaptureFactory = std::function<std::unique_ptr<IRoonCapture>()>;
using RoonControlFactory = std::function<std::unique_ptr<IRoonControl>()>;

class RoonSource final : public IAudioSource {
public:
    explicit RoonSource(RoonConfig cfg, std::filesystem::path data_dir = {},
                        RoonCaptureFactory capture_factory = {},
                        RoonControlFactory control_factory = {});
    ~RoonSource() override;

    std::string_view name() const noexcept override { return "roon"; }
    std::string_view display_name() const noexcept override { return "Roon"; }

    bool initialize() override;
    void shutdown() noexcept override;
    void update_config(RoonConfig cfg, std::filesystem::path data_dir = {});
    roon::RoonCommandResult reconnect();

    void play() override;
    void pause() override;
    void stop() override;
    void next() override;
    void previous() override;
    void pump(RingBuffer& ring) override;

    TrackInfo current_track() const override;
    PlaybackState playback_state() const noexcept override;
    AuthState auth_state() const noexcept override;
    std::string auth_instructions() const override;
    SourceCapabilities capabilities() const noexcept override { return {false, true, false}; }

private:
    RoonConfig config_snapshot() const;
    AuthState setup_state() const noexcept;
    std::string setup_error() const;
    void clear_setup_error();
    void set_setup_error(std::string message);
    bool send_transport(std::string_view control);
    bool start_capture();
    void stop_capture() noexcept;
    void clear_capture() noexcept;
    void start_metadata_polling();
    void stop_metadata_polling() noexcept;
    void metadata_loop(const std::stop_token& tok) noexcept;
    void refresh_metadata() noexcept;

    mutable std::mutex cfg_mu_;
    RoonConfig cfg_;
    std::filesystem::path data_dir_;
    mutable std::mutex sidecar_mu_;
    std::unique_ptr<roon::RoonSidecarProcess> sidecar_;
    RoonCaptureFactory capture_factory_;
    RoonControlFactory control_factory_;
    std::unique_ptr<IRoonCapture> capture_;
    std::mutex control_mu_;
    std::unique_ptr<IRoonControl> control_;
    mutable std::mutex setup_mu_;
    std::string setup_error_;
    std::atomic<bool> setup_error_present_{false};
    mutable std::mutex info_mu_;
    TrackInfo info_{};
    std::jthread metadata_thread_;
    std::atomic<PlaybackState> state_{PlaybackState::stopped};
    std::atomic<bool> initialized_{false};
};

} // namespace fh6::sources
