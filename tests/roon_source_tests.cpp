#include "fh6/sources/roon_source.hpp"
#include "fh6/log.hpp"

#include <atomic>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error{message};
}

void require_contains(const std::string& text, const char* needle, const char* message) {
    if (text.find(needle) == std::string::npos) throw std::runtime_error{message};
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in{path};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

class FakeCapture final : public fh6::sources::IRoonCapture {
public:
    bool start(const fh6::audio::WasapiLoopbackCaptureConfig& cfg) override {
        ++starts;
        last_cfg = cfg;
        return start_ok;
    }
    void stop() noexcept override { ++stops; }
    void clear() noexcept override {
        ++clears;
        pcm.clear();
    }
    fh6::audio::WasapiLoopbackCaptureStatus status() const override { return status_value; }
    std::size_t read_pcm(void* dst, std::size_t bytes) noexcept override {
        if (!dst || bytes == 0 || pcm.empty()) return 0;
        const auto n = std::min(bytes, pcm.size());
        std::memcpy(dst, pcm.data(), n);
        pcm.erase(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(n));
        return n;
    }

    bool start_ok = true;
    int starts    = 0;
    int stops     = 0;
    int clears    = 0;
    std::vector<std::byte> pcm;
    fh6::audio::WasapiLoopbackCaptureConfig last_cfg;
    fh6::audio::WasapiLoopbackCaptureStatus status_value{};
};

class FakeControl final : public fh6::sources::IRoonControl {
public:
    fh6::roon::RoonCommandResult transport(std::string_view control,
                                           std::string_view zone_id) override {
        controls.emplace_back(control);
        zone_ids.emplace_back(zone_id);
        return result;
    }
    fh6::roon::RoonStatus status() override {
        ++status_calls;
        return status_value;
    }

    std::vector<std::string> controls;
    std::vector<std::string> zone_ids;
    std::atomic<int> status_calls{0};
    fh6::roon::RoonCommandResult result{true, 200, {}};
    fh6::roon::RoonStatus status_value{};
};

fh6::TrackInfo wait_for_track(fh6::sources::RoonSource& source, std::string_view title) {
    for (int i = 0; i < 100; ++i) {
        auto track = source.current_track();
        if (track.title == title) return track;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return source.current_track();
}

} // namespace

int run_tests() {
    const auto log_path = std::filesystem::current_path() / "tmp" / "roon-source-tests.log";
    std::filesystem::create_directories(log_path.parent_path());
    std::filesystem::remove(log_path);
    fh6::log::init(log_path);

    fh6::RoonConfig disabled_cfg;
    fh6::sources::RoonSource disabled{disabled_cfg};
    require(!disabled.initialize(), "disabled Roon source should not initialize");

    fh6::RoonConfig cfg;
    cfg.enabled           = true;
    cfg.auto_start_bridge = false;
    fh6::sources::RoonSource source{cfg};

    require(source.name() == "roon", "name should be roon");
    require(source.display_name() == "Roon", "display name should be Roon");
    require(source.initialize(), "enabled placeholder source should initialize");
    require(source.playback_state() == fh6::PlaybackState::stopped, "initial state should stop");
    require(source.auth_state() == fh6::AuthState::needs_auth, "incomplete setup should need auth");
    require_contains(source.auth_instructions(), "Authorize",
                     "instructions should mention authorize");
    require_contains(source.auth_instructions(), "zone", "instructions should mention zone setup");
    require_contains(source.auth_instructions(), "loopback",
                     "instructions should mention loopback setup");

    const auto caps = source.capabilities();
    require(!caps.seek, "placeholder should not advertise seek");
    require(caps.previous, "placeholder should support previous");
    require(!caps.queue, "placeholder should not advertise queue");

    auto track = source.current_track();
    require(track.title.empty(), "placeholder current track title should be empty");
    require(track.artist.empty(), "placeholder current track artist should be empty");

    source.play();
    require(source.playback_state() == fh6::PlaybackState::stopped,
            "play without capture setup should stay stopped");
    source.stop();
    require(source.playback_state() == fh6::PlaybackState::stopped,
            "stop should cache stopped state");

    fh6::RingBuffer ring{4096};
    source.pump(ring);
    require(ring.readable() == 0, "placeholder pump should not write PCM");

    source.shutdown();
    source.shutdown();
    require(source.playback_state() == fh6::PlaybackState::stopped,
            "shutdown should be idempotent");

    fh6::RoonConfig missing_node_cfg;
    missing_node_cfg.enabled     = true;
    missing_node_cfg.node_path   = R"(Z:\fh6-missing-node\node.exe)";
    missing_node_cfg.bridge_path = R"(Z:\fh6-missing-node\index.mjs)";
    fh6::sources::RoonSource missing_node{missing_node_cfg};
    require(missing_node.initialize(), "node setup errors should keep Roon registered");
    require(missing_node.auth_state() == fh6::AuthState::error,
            "missing Node should surface as auth/setup error");
    require_contains(missing_node.auth_instructions(), "Node.js",
                     "missing Node instructions should be actionable");
    auto missing_node_reconnect = missing_node.reconnect();
    require(!missing_node_reconnect.ok, "reconnect should report sidecar restart failures");
    require_contains(missing_node_reconnect.error, "Node.js",
                     "reconnect should surface owned sidecar startup errors");

    fh6::RoonConfig ready_cfg;
    ready_cfg.enabled           = true;
    ready_cfg.auto_start_bridge = false;
    ready_cfg.selected_zone_id  = "zone-1";
    ready_cfg.render_loopback_endpoint_id = "device-1";
    ready_cfg.latency_ms        = 123;

    fh6::RoonConfig offline_cfg  = ready_cfg;
    offline_cfg.metadata_poll_ms = 10;
    FakeControl* offline_control = nullptr;
    fh6::sources::RoonSource offline_source{
        offline_cfg,
        {},
        [] { return std::make_unique<FakeCapture>(); },
        [&] {
            auto fake                = std::make_unique<FakeControl>();
            fake->status_value.error = "sidecar status request failed";
            offline_control          = fake.get();
            return fake;
        }};
    require(offline_source.initialize(), "offline Roon source should initialize");
    for (int i = 0; i < 100; ++i) {
        if (offline_control && offline_control->status_calls > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    require(offline_control != nullptr && offline_control->status_calls > 0,
            "offline auth test should poll status");
    require(offline_source.auth_state() != fh6::AuthState::authenticated,
            "offline Roon status should not authenticate from config alone");
    require_contains(offline_source.auth_instructions(), "sidecar",
                     "offline Roon status should remain actionable");
    offline_source.shutdown();

    FakeControl* control         = nullptr;
    FakeCapture* controlled_fake = nullptr;
    fh6::sources::RoonSource controlled_source{ready_cfg,
                                               {},
                                               [&] {
                                                   auto capture = std::make_unique<FakeCapture>();
                                                   controlled_fake = capture.get();
                                                   return capture;
                                               },
                                               [&] {
                                                   auto fake = std::make_unique<FakeControl>();
                                                   control   = fake.get();
                                                   return fake;
                                               }};
    require(controlled_source.initialize(), "controlled Roon source should initialize");
    controlled_source.play();
    controlled_source.pause();
    controlled_source.play();
    controlled_source.next();
    controlled_source.previous();
    controlled_source.stop();
    require(control != nullptr, "RoonSource should create a control client");
    require((control->controls ==
             std::vector<std::string>{"play", "pause", "play", "next", "previous", "stop"}),
            "RoonSource should forward transport controls to Roon");
    require((control->zone_ids ==
             std::vector<std::string>{"zone-1", "zone-1", "zone-1", "zone-1", "zone-1", "zone-1"}),
            "RoonSource should send the selected zone id with transport controls");
    require(controlled_fake != nullptr && controlled_fake->starts >= 2,
            "controlled play should still start capture");

    fh6::RoonConfig metadata_cfg  = ready_cfg;
    metadata_cfg.metadata_poll_ms = 10;
    FakeControl* metadata_control = nullptr;
    fh6::sources::RoonSource metadata_source{
        metadata_cfg,
        {},
        [] { return std::make_unique<FakeCapture>(); },
        [&] {
            auto fake                           = std::make_unique<FakeControl>();
            fake->status_value.ok               = true;
            fake->status_value.pairing_state    = "authorized";
            fake->status_value.selected_zone_id = "zone-1";
            fake->status_value.now_playing      = fh6::roon::RoonNowPlaying{
                "Road Song", "The Drivers", "Horizon Radio", "/api/source/roon/artwork/current",
                245000,      67000};
            metadata_control = fake.get();
            return fake;
        }};
    require(metadata_source.initialize(), "metadata Roon source should initialize");
    const auto roon_track = wait_for_track(metadata_source, "Road Song");
    require(metadata_control != nullptr && metadata_control->status_calls > 0,
            "metadata poller should query Roon status");
    require(roon_track.title == "Road Song", "Roon metadata poller should update track title");
    require(roon_track.artist == "The Drivers", "Roon metadata poller should update artist");
    require(roon_track.album == "Horizon Radio", "Roon metadata poller should update album");
    require(roon_track.position_ms == 67000, "Roon metadata poller should update position");
    metadata_source.shutdown();

    FakeCapture* fake                 = nullptr;
    FakeControl* capture_auth_control = nullptr;
    fh6::sources::RoonSource capture_source{ready_cfg,
                                            {},
                                            [&] {
                                                auto capture = std::make_unique<FakeCapture>();
                                                fake         = capture.get();
                                                return capture;
                                            },
                                            [&] {
                                                auto control = std::make_unique<FakeControl>();
                                                control->status_value.ok            = true;
                                                control->status_value.pairing_state = "authorized";
                                                control->status_value.selected_zone_id = "zone-1";
                                                capture_auth_control = control.get();
                                                return control;
                                            }};
    require(capture_source.initialize(), "ready Roon source should initialize");
    for (int i = 0; i < 100; ++i) {
        if (capture_auth_control && capture_auth_control->status_calls > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    require(capture_source.auth_state() == fh6::AuthState::authenticated,
            "complete Roon setup should authenticate");

    capture_source.play();
    require(fake != nullptr, "play should create a capture worker");
    require(fake->starts == 1, "play should start capture");
    require(fake->last_cfg.device_id == "device-1", "capture should use configured device id");
    require(fake->last_cfg.latency_ms == 123, "capture should use configured latency");

    const std::uint32_t marker = 0xAABBCCDDu;
    fake->pcm.resize(sizeof(marker));
    std::memcpy(fake->pcm.data(), &marker, sizeof(marker));
    fh6::RingBuffer capture_ring{4096};
    capture_source.pump(capture_ring);
    require(capture_ring.readable() == sizeof(marker), "pump should drain capture PCM into ring");

    std::vector<std::byte> stale_pcm(98304, std::byte{0x11});
    fh6::RingBuffer delayed_ring{131072};
    require(delayed_ring.write(stale_pcm.data(), stale_pcm.size()) == stale_pcm.size(),
            "test should seed stale downstream Roon PCM");
    const std::uint32_t fresh_marker = 0x11223344u;
    fake->pcm.resize(sizeof(fresh_marker));
    std::memcpy(fake->pcm.data(), &fresh_marker, sizeof(fresh_marker));
    fake->status_value.queued_bytes = sizeof(fresh_marker);
    capture_source.pump(delayed_ring);
    std::uint32_t heard_marker = 0;
    require(delayed_ring.read(&heard_marker, sizeof(heard_marker)) == sizeof(heard_marker),
            "Roon pump should leave fresh PCM readable after dropping stale downstream backlog");
    require(heard_marker == fresh_marker,
            "Roon pump should drop stale downstream PCM when live backlog exceeds capture latency");

    capture_source.pause();
    require(fake->stops == 1, "pause should stop capture");
    require(fake->clears == 1, "pause should clear stale capture PCM");

    capture_source.play();
    capture_source.next();
    require(fake->clears >= 2, "next should clear stale capture PCM");
    capture_source.previous();
    require(fake->clears >= 3, "previous should clear stale capture PCM");
    capture_source.stop();
    require(fake->stops >= 2, "stop should stop capture");
    require(capture_source.playback_state() == fh6::PlaybackState::stopped,
            "stop should cache stopped state");

    fh6::RoonConfig changed_cfg   = ready_cfg;
    changed_cfg.render_loopback_endpoint_id = "device-2";
    changed_cfg.latency_ms        = 456;
    capture_source.update_config(changed_cfg);
    capture_source.play();
    require(fake->starts >= 4, "play after config update should restart capture");
    require(fake->last_cfg.device_id == "device-2",
            "RoonSource should use updated capture device id");
    require(fake->last_cfg.latency_ms == 456, "RoonSource should use updated latency");

    FakeCapture* failing_fake = nullptr;
    fh6::sources::RoonSource failing_capture_source{ready_cfg,
                                                    {},
                                                    [&] {
                                                        auto capture =
                                                            std::make_unique<FakeCapture>();
                                                        capture->start_ok = false;
                                                        capture->status_value.error =
                                                            "capture failed";
                                                        failing_fake = capture.get();
                                                        return capture;
                                                    },
                                                    [] { return std::make_unique<FakeControl>(); }};
    require(failing_capture_source.initialize(), "capture failure source should initialize");
    failing_capture_source.play();
    require(failing_fake != nullptr && failing_fake->starts == 1,
            "play should try to start failing capture");
    require(failing_capture_source.auth_state() == fh6::AuthState::error,
            "capture start failure should surface as auth error");
    require_contains(failing_capture_source.auth_instructions(), "capture failed",
                     "capture failure should be actionable");

    fh6::log::shutdown();
    const auto log_text = read_text(log_path);
    require_contains(log_text, "[roon] source initialized",
                     "Roon source should log initialization");
    require_contains(log_text, "[roon] play requested", "Roon source should log play requests");
    require_contains(log_text, "[roon] capture started",
                     "Roon source should log capture start success");
    require_contains(log_text, "[roon] capture start failed",
                     "Roon source should log capture start failure");
    require_contains(log_text, "[roon] source shutdown", "Roon source should log shutdown");
    std::filesystem::remove(log_path);

    return 0;
}

int main() {
    try {
        return run_tests();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
