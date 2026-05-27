#include "fh6/source_registration.hpp"
#include "fh6/audio_source.hpp"
#include "fh6/audio_source_manager.hpp"
#include "fh6/config.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error{message};
}

class TestSource final : public fh6::IAudioSource {
public:
    explicit TestSource(std::string name) : name_{std::move(name)} {}

    std::string_view name() const noexcept override { return name_; }
    std::string_view display_name() const noexcept override { return name_; }

    bool initialize() override { return true; }
    void shutdown() noexcept override { state_ = fh6::PlaybackState::stopped; }

    void play() override { state_ = fh6::PlaybackState::playing; }
    void pause() override { state_ = fh6::PlaybackState::paused; }
    void stop() override { state_ = fh6::PlaybackState::stopped; }

    fh6::TrackInfo current_track() const override { return {}; }
    fh6::PlaybackState playback_state() const noexcept override { return state_; }
    fh6::AuthState auth_state() const noexcept override { return fh6::AuthState::none_required; }
    fh6::SourceCapabilities capabilities() const noexcept override { return {}; }

private:
    std::string name_;
    fh6::PlaybackState state_ = fh6::PlaybackState::stopped;
};

fh6::RoonConfig enabled_roon_config() {
    fh6::RoonConfig cfg;
    cfg.enabled = true;
    cfg.auto_start_bridge = false;
    return cfg;
}

} // namespace

int main() {
    fh6::AudioSourceManager mgr{4096};

    auto roon_cfg = enabled_roon_config();
    fh6::sync_roon_source(mgr, roon_cfg);
    auto* roon = mgr.find("roon");
    require(roon != nullptr, "enabled Roon config should register the roon source");
    require(roon->name() == "roon", "registered source should use the roon source name");

    fh6::sync_roon_source(mgr, roon_cfg);
    require(mgr.find("roon") == roon, "syncing an already registered Roon source should not replace it");
    require(roon->auth_state() == fh6::AuthState::needs_auth,
            "incomplete Roon config should need setup");

    roon_cfg.selected_zone_id = "zone-1";
    roon_cfg.render_loopback_endpoint_id = "device-1";
    fh6::sync_roon_source(mgr, roon_cfg);
    require(mgr.find("roon") == roon, "updating Roon config should keep the registered source");
    require(roon->auth_state() == fh6::AuthState::authenticated,
            "syncing updated Roon config should update the live source setup state");

    require(mgr.switch_to("roon"), "default_source roon should be switchable after registration");
    require(mgr.active() == roon, "switch_to roon should make Roon active");

    const std::uint32_t marker = 0x12345678u;
    require(mgr.ring().write(&marker, sizeof(marker)) == sizeof(marker), "test should seed ring data");
    require(mgr.ring().readable() == sizeof(marker), "seeded ring data should be readable before unregister");

    roon_cfg.enabled = false;
    fh6::sync_roon_source(mgr, roon_cfg);
    require(mgr.find("roon") == nullptr, "disabled Roon config should unregister the roon source");
    require(mgr.active() == nullptr, "unregistering active Roon should clear the active source");
    require(mgr.ring().readable() == 0, "unregistering active Roon should drain stale PCM");

    mgr.register_source(std::make_unique<TestSource>("local_files"));
    require(!mgr.switch_to("roon"), "switch_to roon should fail when Roon is not registered");
    require(mgr.switch_to("local_files"), "fallback source should remain switchable without Roon");
    require(mgr.active() != nullptr && mgr.active()->name() == "local_files",
            "fallback source should become active when Roon is unavailable");

    return 0;
}
