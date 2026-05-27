#pragma once

#include "fh6/audio_source.hpp"
#include "fh6/config.hpp"
#include "fh6/roon/roon_sidecar_process.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>

namespace fh6::sources {

class RoonSource final : public IAudioSource {
public:
    explicit RoonSource(RoonConfig cfg, std::filesystem::path data_dir = {});
    ~RoonSource() override;

    std::string_view name() const noexcept override { return "roon"; }
    std::string_view display_name() const noexcept override { return "Roon"; }

    bool initialize() override;
    void shutdown() noexcept override;

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
    AuthState setup_state() const noexcept;
    std::string setup_error() const;

    RoonConfig cfg_;
    std::filesystem::path data_dir_;
    std::unique_ptr<roon::RoonSidecarProcess> sidecar_;
    mutable std::mutex setup_mu_;
    std::string setup_error_;
    std::atomic<bool> setup_error_present_{false};
    TrackInfo info_{};
    std::atomic<PlaybackState> state_{PlaybackState::stopped};
    std::atomic<bool> initialized_{false};
};

} // namespace fh6::sources
