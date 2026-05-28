#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fh6::audio {

struct WasapiRenderDevice {
    std::string id;
    std::string name;
    bool is_default = false;
};

struct WasapiLoopbackCaptureConfig {
    std::string device_id;
    uint32_t latency_ms = 250;
    uint32_t queue_ms   = 1000;
};

struct WasapiLoopbackCaptureStatus {
    bool running = false;
    std::string error;
    float peak               = 0.0f;
    std::size_t queued_bytes = 0;
};

class WasapiLoopbackCapture {
public:
    WasapiLoopbackCapture();
    ~WasapiLoopbackCapture();

    WasapiLoopbackCapture(const WasapiLoopbackCapture&)            = delete;
    WasapiLoopbackCapture& operator=(const WasapiLoopbackCapture&) = delete;

    bool start(const WasapiLoopbackCaptureConfig& cfg);
    void stop() noexcept;
    WasapiLoopbackCaptureStatus status() const;
    std::size_t read_pcm(void* dst, std::size_t bytes) noexcept;
    void clear() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::vector<WasapiRenderDevice> enumerate_render_devices();

} // namespace fh6::audio
