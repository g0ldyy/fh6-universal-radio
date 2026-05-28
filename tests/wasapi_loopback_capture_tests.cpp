#include "fh6/audio/wasapi_loopback_capture.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error{message};
}

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error{message};
}

} // namespace

int main() {
    try {
        const auto devices = fh6::audio::enumerate_render_devices();

        std::unordered_set<std::string> seen_ids;
        int default_count = 0;
        for (const auto& device : devices) {
            require(!device.id.empty(), "render device id should be stable and non-empty");
            require(!device.name.empty(), "render device name should be friendly and non-empty");
            require(seen_ids.insert(device.id).second, "render device ids should be unique");
            if (device.is_default) ++default_count;
        }

        require(default_count <= 1, "at most one render device should be marked default");

        fh6::audio::WasapiLoopbackCapture capture;
        fh6::audio::WasapiLoopbackCaptureConfig invalid;
        invalid.device_id = "not-a-real-wasapi-render-endpoint";
        require(!capture.start(invalid), "invalid render device should fail to start");
        require(!capture.status().running, "failed capture should not report running");
        require(!capture.status().error.empty(),
                "failed capture should report an actionable error");
        capture.stop();
        capture.stop();
        require(capture.read_pcm(nullptr, 0) == 0, "empty reads should be non-blocking no-ops");

        if (!devices.empty()) {
            fh6::audio::WasapiLoopbackCaptureConfig cfg;
            cfg.device_id  = devices.front().id;
            cfg.latency_ms = 100;
            cfg.queue_ms   = 250;
            require(capture.start(cfg),
                    "valid render device should start: " + capture.status().error);
            require(capture.status().running, "started capture should report running");

            std::vector<std::byte> scratch(4096);
            capture.read_pcm(scratch.data(), scratch.size());
            capture.clear();
            capture.stop();
            require(!capture.status().running, "stopped capture should not report running");
        }

        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "unknown wasapi_loopback_capture_tests failure\n";
        return 1;
    }
}
