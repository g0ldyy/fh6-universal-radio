#include "fh6/audio/wasapi_loopback_capture.hpp"
#include "fh6/log.hpp"
#include "fh6/ring_buffer.hpp"

#include <windows.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#define MA_NO_DEVICE_IO
#define MA_NO_GENERATION
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244) // narrowing inside miniaudio's header
#endif
#include <miniaudio.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

namespace fh6::audio {
namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t kTargetSampleRate = 48000;
constexpr uint32_t kTargetChannels   = 2;
constexpr uint32_t kTargetFrameBytes = kTargetChannels * sizeof(int16_t);
constexpr auto kStartReadyTimeout    = std::chrono::seconds{3};

struct ComApartment {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ~ComApartment() {
        if (hr == S_OK || hr == S_FALSE) CoUninitialize();
    }

    bool usable() const noexcept { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

struct CoTaskMemDeleter {
    template <class T> void operator()(T* value) const noexcept {
        if (value) CoTaskMemFree(value);
    }
};

using CoTaskMemFormat = std::unique_ptr<WAVEFORMATEX, CoTaskMemDeleter>;

struct MixFormat {
    ma_format format     = ma_format_unknown;
    uint32_t channels    = 0;
    uint32_t sample_rate = 0;
    uint32_t block_align = 0;
};

std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) return {};
    const int needed =
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (needed <= 0) return {};

    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(),
                        needed);
    return out;
}

std::string hr_error(std::string_view context, HRESULT hr) {
    return std::string{context} + " failed with HRESULT " + std::to_string(static_cast<long>(hr));
}

std::string_view format_name(ma_format format) noexcept {
    switch (format) {
        case ma_format_f32: return "f32";
        case ma_format_s16: return "s16";
        case ma_format_s24: return "s24";
        case ma_format_s32: return "s32";
        default: return "unknown";
    }
}

ComPtr<IMMDevice> open_render_device(IMMDeviceEnumerator* enumerator, std::string_view id,
                                     std::string& error) {
    ComPtr<IMMDevice> device;
    HRESULT hr = E_FAIL;
    if (id.empty()) {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr)) error = hr_error("GetDefaultAudioEndpoint", hr);
        return device;
    }

    const auto wide_id = utf8_to_wide(id);
    if (wide_id.empty()) {
        error = "Capture device id is not valid UTF-8.";
        return {};
    }
    hr = enumerator->GetDevice(wide_id.c_str(), &device);
    if (FAILED(hr)) error = "Capture device was not found. Re-select the Roon capture device.";
    return device;
}

std::optional<ma_format> ma_format_from_wave(const WAVEFORMATEX& wf) {
    if (wf.wFormatTag == WAVE_FORMAT_IEEE_FLOAT && wf.wBitsPerSample == 32) return ma_format_f32;
    if (wf.wFormatTag == WAVE_FORMAT_PCM) {
        if (wf.wBitsPerSample == 16) return ma_format_s16;
        if (wf.wBitsPerSample == 24) return ma_format_s24;
        if (wf.wBitsPerSample == 32) return ma_format_s32;
    }
    if (wf.wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        wf.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        return std::nullopt;
    }

    const auto& ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(wf);
    const auto bits =
        ext.Samples.wValidBitsPerSample ? ext.Samples.wValidBitsPerSample : wf.wBitsPerSample;
    if (IsEqualGUID(ext.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && bits == 32)
        return ma_format_f32;
    if (IsEqualGUID(ext.SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
        if (bits == 16) return ma_format_s16;
        if (bits == 24) return ma_format_s24;
        if (bits == 32) return ma_format_s32;
    }
    return std::nullopt;
}

std::optional<MixFormat> describe_mix_format(const WAVEFORMATEX& wf) {
    auto format = ma_format_from_wave(wf);
    if (!format || wf.nChannels == 0 || wf.nSamplesPerSec == 0 || wf.nBlockAlign == 0)
        return std::nullopt;
    return MixFormat{*format, wf.nChannels, wf.nSamplesPerSec, wf.nBlockAlign};
}

std::size_t queue_bytes(uint32_t queue_ms) {
    const auto ms = std::clamp<uint32_t>(queue_ms, 100, 5000);
    return (static_cast<std::size_t>(kTargetSampleRate) * kTargetFrameBytes * ms) / 1000;
}

void write_queue(RingBuffer& queue, const std::byte* data, std::size_t bytes) {
    if (!data || bytes == 0) return;
    const auto capacity = queue.capacity() - (queue.capacity() % kTargetFrameBytes);
    if (bytes > capacity) {
        data  += bytes - capacity;
        bytes  = capacity;
    }
    if (queue.write(data, bytes) != bytes) {
        queue.drain();
        queue.write(data, bytes);
    }
}

float peak_s16(const std::byte* data, std::size_t bytes) {
    const auto* samples = reinterpret_cast<const int16_t*>(data);
    const auto count    = bytes / sizeof(int16_t);
    int peak            = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const int sample = samples[i];
        const int mag    = sample == INT16_MIN ? 32768 : std::abs(sample);
        peak             = std::max(peak, mag);
    }
    return static_cast<float>(peak) / 32768.0f;
}

bool convert_packet(ma_data_converter& converter, const MixFormat& format, const BYTE* input,
                    UINT32 frames, std::vector<std::byte>& output) {
    ma_uint64 expected = 0;
    if (ma_data_converter_get_expected_output_frame_count(&converter, frames, &expected) !=
        MA_SUCCESS) {
        expected = ((static_cast<ma_uint64>(frames) * kTargetSampleRate) / format.sample_rate) + 32;
    }
    output.resize(static_cast<std::size_t>(expected) * kTargetFrameBytes);

    ma_uint64 in_frames  = frames;
    ma_uint64 out_frames = expected;
    const auto result    = ma_data_converter_process_pcm_frames(&converter, input, &in_frames,
                                                                output.data(), &out_frames);
    if (result != MA_SUCCESS || out_frames == 0) {
        output.clear();
        return false;
    }
    output.resize(static_cast<std::size_t>(out_frames) * kTargetFrameBytes);
    return true;
}

} // namespace

struct WasapiLoopbackCapture::Impl {
    mutable std::mutex mu;
    std::shared_ptr<RingBuffer> queue;
    std::jthread worker;
    std::atomic<bool> running{false};
    std::atomic<float> peak{0.0f};
    std::string error;

    void set_error(std::string message) {
        {
            std::scoped_lock lk{mu};
            error = message;
        }
        log::warn("[wasapi] {}", message);
    }

    std::string current_error() const {
        std::scoped_lock lk{mu};
        return error;
    }

    std::shared_ptr<RingBuffer> queue_snapshot() const {
        std::scoped_lock lk{mu};
        return queue;
    }

    void run(const std::stop_token& stop, const WasapiLoopbackCaptureConfig& cfg,
             const std::shared_ptr<RingBuffer>& pcm_queue, std::promise<bool> ready) {
        ComApartment com;
        if (!com.usable()) {
            set_error("COM initialization failed for WASAPI capture.");
            ready.set_value(false);
            return;
        }

        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) {
            set_error(hr_error("CoCreateInstance(MMDeviceEnumerator)", hr));
            ready.set_value(false);
            return;
        }

        std::string setup_error;
        auto device = open_render_device(enumerator.Get(), cfg.device_id, setup_error);
        if (!device) {
            set_error(setup_error.empty() ? "Capture device could not be opened." : setup_error);
            ready.set_value(false);
            return;
        }

        ComPtr<IAudioClient> client;
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(client.GetAddressOf()));
        if (FAILED(hr)) {
            set_error(hr_error("IAudioClient activation", hr));
            ready.set_value(false);
            return;
        }

        WAVEFORMATEX* raw_format = nullptr;
        hr                       = client->GetMixFormat(&raw_format);
        CoTaskMemFormat wave_format{raw_format};
        if (FAILED(hr) || !wave_format) {
            set_error(hr_error("GetMixFormat", hr));
            ready.set_value(false);
            return;
        }

        const auto format = describe_mix_format(*wave_format);
        if (!format) {
            set_error("WASAPI mix format is unsupported for Roon loopback capture.");
            ready.set_value(false);
            return;
        }
        log::info("[wasapi] capture mix format: {}ch {}Hz {} block_align={}", format->channels,
                  format->sample_rate, format_name(format->format), format->block_align);
        log::info("[wasapi] conversion path: miniaudio {}ch {}Hz {} -> 2ch 48000Hz s16",
                  format->channels, format->sample_rate, format_name(format->format));

        ma_data_converter_config converter_cfg =
            ma_data_converter_config_init(format->format, ma_format_s16, format->channels,
                                          kTargetChannels, format->sample_rate, kTargetSampleRate);
        converter_cfg.resampling.algorithm = ma_resample_algorithm_linear;
        ma_data_converter converter{};
        if (ma_data_converter_init(&converter_cfg, nullptr, &converter) != MA_SUCCESS) {
            set_error("Failed to initialize PCM converter for WASAPI loopback.");
            ready.set_value(false);
            return;
        }
        const auto cleanup_converter =
            std::unique_ptr<ma_data_converter, void (*)(ma_data_converter*)>{
                &converter, [](ma_data_converter* c) { ma_data_converter_uninit(c, nullptr); }};

        const auto buffer_hns =
            static_cast<REFERENCE_TIME>(std::clamp<uint32_t>(cfg.latency_ms, 50, 2000)) * 10000;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, buffer_hns,
                                0, wave_format.get(), nullptr);
        if (FAILED(hr)) {
            set_error(hr_error("IAudioClient::Initialize(loopback)", hr));
            ready.set_value(false);
            return;
        }

        ComPtr<IAudioCaptureClient> capture;
        hr = client->GetService(__uuidof(IAudioCaptureClient),
                                reinterpret_cast<void**>(capture.GetAddressOf()));
        if (FAILED(hr)) {
            set_error(hr_error("IAudioClient::GetService(IAudioCaptureClient)", hr));
            ready.set_value(false);
            return;
        }

        hr = client->Start();
        if (FAILED(hr)) {
            set_error(hr_error("IAudioClient::Start", hr));
            ready.set_value(false);
            return;
        }

        running.store(true, std::memory_order_release);
        ready.set_value(true);
        log::info("[wasapi] capture worker started");

        std::vector<std::byte> silence;
        std::vector<std::byte> converted;
        bool saw_signal           = false;
        auto last_silence_warning = std::chrono::steady_clock::now();
        while (!stop.stop_requested()) {
            UINT32 packet_frames = 0;
            hr                   = capture->GetNextPacketSize(&packet_frames);
            if (FAILED(hr)) {
                set_error(hr_error("IAudioCaptureClient::GetNextPacketSize", hr));
                break;
            }
            if (packet_frames == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
                continue;
            }

            while (packet_frames > 0 && !stop.stop_requested()) {
                BYTE* data    = nullptr;
                UINT32 frames = 0;
                DWORD flags   = 0;
                hr            = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) {
                    set_error(hr_error("IAudioCaptureClient::GetBuffer", hr));
                    packet_frames = 0;
                    break;
                }

                const auto input_bytes = static_cast<std::size_t>(frames) * format->block_align;
                const BYTE* input      = data;
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0U) {
                    silence.assign(input_bytes, std::byte{0});
                    input = reinterpret_cast<const BYTE*>(silence.data());
                }

                if (convert_packet(converter, *format, input, frames, converted)) {
                    const auto level = peak_s16(converted.data(), converted.size());
                    peak.store(level, std::memory_order_release);
                    saw_signal     = saw_signal || level > 0.01f;
                    const auto now = std::chrono::steady_clock::now();
                    if (!saw_signal && now - last_silence_warning > std::chrono::seconds{5}) {
                        log::warn("[wasapi] capture is silent; verify the selected Roon output "
                                  "and Windows capture device");
                        last_silence_warning = now;
                    }
                    write_queue(*pcm_queue, converted.data(), converted.size());
                }

                capture->ReleaseBuffer(frames);
                hr = capture->GetNextPacketSize(&packet_frames);
                if (FAILED(hr)) {
                    set_error(hr_error("IAudioCaptureClient::GetNextPacketSize", hr));
                    packet_frames = 0;
                }
            }
        }

        client->Stop();
        running.store(false, std::memory_order_release);
        log::info("[wasapi] capture worker stopped");
    }
};

WasapiLoopbackCapture::WasapiLoopbackCapture() : impl_{std::make_unique<Impl>()} {}
WasapiLoopbackCapture::~WasapiLoopbackCapture() { stop(); }

bool WasapiLoopbackCapture::start(const WasapiLoopbackCaptureConfig& cfg) {
    stop();
    log::info("[wasapi] capture start requested device_id={} latency_ms={} queue_ms={}",
              cfg.device_id, cfg.latency_ms, cfg.queue_ms);
    auto queue = std::make_shared<RingBuffer>(queue_bytes(cfg.queue_ms));
    {
        std::scoped_lock lk{impl_->mu};
        impl_->queue = queue;
        impl_->error.clear();
    }
    impl_->peak.store(0.0f, std::memory_order_release);

    std::promise<bool> ready;
    auto started  = ready.get_future();
    impl_->worker = std::jthread{
        [this, cfg, queue, ready = std::move(ready)](const std::stop_token& stop) mutable {
            impl_->run(stop, cfg, queue, std::move(ready));
        }};
    if (started.wait_for(kStartReadyTimeout) != std::future_status::ready) {
        impl_->set_error("Timed out waiting for WASAPI capture worker startup.");
        if (impl_->worker.joinable()) impl_->worker.request_stop();
        impl_->running.store(false, std::memory_order_release);
        return false;
    }
    const bool ok = started.get();
    log::info("[wasapi] capture start result={}", ok);
    return ok;
}

void WasapiLoopbackCapture::stop() noexcept {
    if (impl_->worker.joinable()) {
        log::info("[wasapi] capture stop requested");
        impl_->worker.request_stop();
        impl_->worker.join();
    }
    impl_->running.store(false, std::memory_order_release);
}

WasapiLoopbackCaptureStatus WasapiLoopbackCapture::status() const {
    auto queue = impl_->queue_snapshot();
    return WasapiLoopbackCaptureStatus{
        impl_->running.load(std::memory_order_acquire),
        impl_->current_error(),
        impl_->peak.load(std::memory_order_acquire),
        queue ? queue->readable() : 0,
    };
}

std::size_t WasapiLoopbackCapture::read_pcm(void* dst, std::size_t bytes) noexcept {
    if (!dst || bytes == 0) return 0;
    auto queue = impl_->queue_snapshot();
    return queue ? queue->read(dst, bytes) : 0;
}

void WasapiLoopbackCapture::clear() noexcept {
    auto queue = impl_->queue_snapshot();
    if (queue) queue->drain();
}

} // namespace fh6::audio
