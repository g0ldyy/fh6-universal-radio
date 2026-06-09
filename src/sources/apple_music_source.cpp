#include "fh6/sources/apple_music_source.hpp"
#include "fh6/log.hpp"

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <propsys.h>
#include <tlhelp32.h>
#include <windows.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <cstring>
#include <cwctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fh6::sources {

namespace {

namespace media = winrt::Windows::Media::Control;
namespace streams = winrt::Windows::Storage::Streams;
using namespace std::chrono_literals;

constexpr uint32_t kOutRate = 48000;
constexpr uint32_t kOutChannels = 2;
constexpr uint32_t kFrameBytes = kOutChannels * sizeof(int16_t);
constexpr uint64_t kPcmBytesPerSec = uint64_t{kOutRate} * kFrameBytes;

void normalize_artist_album(std::string& artist, std::string& album, bool keep_album) {
    constexpr std::array<std::string_view, 3> kSeparators = {
        " \xE2\x80\x94 ", // em dash
        " \xE2\x80\x93 ", // en dash
        " - "
    };
    std::size_t pos = std::string::npos;
    std::size_t sep_len = 0;
    for (auto sep : kSeparators) {
        pos = artist.find(sep);
        if (pos != std::string::npos) {
            sep_len = sep.size();
            break;
        }
    }
    if (pos == std::string::npos) {
        if (!keep_album) album.clear();
        return;
    }

    auto trailing = artist.substr(pos + sep_len);
    if (trailing.empty()) return;
    if (keep_album) {
        if (album.empty()) {
            album = std::move(trailing);
        } else if (album != trailing) {
            return;
        }
    } else {
        album.clear();
    }
    artist.erase(pos);
}
template <class T> void release_com(T*& p) noexcept {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

WAVEFORMATEX* alloc_pcm_format(uint32_t sample_rate) noexcept {
    auto* wf = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
    if (!wf) return nullptr;
    std::memset(wf, 0, sizeof(WAVEFORMATEX));
    wf->wFormatTag = WAVE_FORMAT_PCM;
    wf->nChannels = 2;
    wf->nSamplesPerSec = sample_rate;
    wf->wBitsPerSample = 16;
    wf->nBlockAlign = wf->nChannels * wf->wBitsPerSample / 8;
    wf->nAvgBytesPerSec = wf->nSamplesPerSec * wf->nBlockAlign;
    return wf;
}

int16_t clamp_s16(float v) noexcept {
    v = std::clamp(v, -1.0f, 1.0f);
    return static_cast<int16_t>(std::lrintf(v * 32767.0f));
}

bool is_extensible_subformat(const WAVEFORMATEX& wf, const GUID& subformat) noexcept {
    if (wf.wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        wf.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
        return false;
    const auto& ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(wf);
    return IsEqualGUID(ext.SubFormat, subformat);
}

bool is_float_format(const WAVEFORMATEX& wf) noexcept {
    return wf.wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
           is_extensible_subformat(wf, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
}

bool is_pcm_format(const WAVEFORMATEX& wf) noexcept {
    return wf.wFormatTag == WAVE_FORMAT_PCM ||
           is_extensible_subformat(wf, KSDATAFORMAT_SUBTYPE_PCM);
}

bool is_apple_music_process_name(const wchar_t* name) noexcept {
    return _wcsicmp(name, L"AppleMusic.exe") == 0 ||
           _wcsicmp(name, L"Music.UI.exe") == 0 ||
           _wcsicmp(name, L"iTunes.exe") == 0 ||
           _wcsicmp(name, L"AMPLibraryAgent.exe") == 0;
}

std::vector<DWORD> find_apple_music_pids() noexcept {
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            if (is_apple_music_process_name(entry.szExeFile)) pids.push_back(entry.th32ProcessID);
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return pids;
}

bool contains_pid(const std::vector<DWORD>& pids, DWORD pid) noexcept {
    return std::find(pids.begin(), pids.end(), pid) != pids.end();
}

uint64_t ms_from_timespan(winrt::Windows::Foundation::TimeSpan const& ts) noexcept {
    using namespace std::chrono;
    const auto ms = duration_cast<milliseconds>(ts);
    return ms.count() > 0 ? static_cast<uint64_t>(ms.count()) : 0;
}

bool is_apple_music_session_id(std::string_view id) noexcept {
    return id.find("AppleMusic") != std::string_view::npos ||
           id.find("AppleInc.AppleMusic") != std::string_view::npos ||
           id.find("iTunes") != std::string_view::npos;
}

float sample_as_float(const std::byte* frame, uint16_t ch, const WAVEFORMATEX& wf) noexcept {
    const auto* p = frame + (size_t{ch} * wf.wBitsPerSample / 8u);
    if (is_float_format(wf) && wf.wBitsPerSample == 32) {
        float v = 0.0f;
        std::memcpy(&v, p, sizeof(v));
        return std::isfinite(v) ? v : 0.0f;
    }
    if (is_pcm_format(wf) && wf.wBitsPerSample == 16) {
        int16_t v = 0;
        std::memcpy(&v, p, sizeof(v));
        return static_cast<float>(v) / 32768.0f;
    }
    if (is_pcm_format(wf) && wf.wBitsPerSample == 24) {
        int32_t v = (int32_t(p[0]) << 8) | (int32_t(p[1]) << 16) | (int32_t(p[2]) << 24);
        return static_cast<float>(v) / 2147483648.0f;
    }
    if (is_pcm_format(wf) && wf.wBitsPerSample == 32) {
        int32_t v = 0;
        std::memcpy(&v, p, sizeof(v));
        return static_cast<float>(v) / 2147483648.0f;
    }
    return 0.0f;
}

std::string narrow(const winrt::hstring& s) {
    return winrt::to_string(s);
}

std::wstring widen_ascii(std::string_view s) {
    std::wstring out;
    out.reserve(s.size());
    for (char ch : s) out.push_back(static_cast<unsigned char>(ch));
    return out;
}

std::wstring lower_copy(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
    return s;
}

std::wstring device_friendly_name(IMMDevice* device) {
    if (!device) return {};
    IPropertyStore* props = nullptr;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &props)) || !props) return {};

    PROPVARIANT value{};
    PropVariantInit(&value);
    std::wstring name;
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR &&
        value.pwszVal) {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    props->Release();
    return name;
}

bool device_name_matches(std::wstring_view name, std::wstring_view needle) {
    if (needle.empty()) return false;
    const auto lowered_name = lower_copy(std::wstring{name});
    const auto lowered_needle = lower_copy(std::wstring{needle});
    return lowered_name.find(lowered_needle) != std::wstring::npos;
}

class AudioActivationHandler final : public IActivateAudioInterfaceCompletionHandler {
public:
    AudioActivationHandler() : done_{CreateEventW(nullptr, TRUE, FALSE, nullptr)} {}
    ~AudioActivationHandler() {
        if (done_) CloseHandle(done_);
    }

    HRESULT wait(IAudioClient** out) {
        if (!done_) return E_OUTOFMEMORY;
        WaitForSingleObject(done_, 10'000);
        if (FAILED(hr_)) return hr_;
        if (!client_) return E_FAIL;
        *out = client_;
        client_ = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** obj) override {
        if (!obj) return E_POINTER;
        if (iid == __uuidof(IUnknown) ||
            iid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
            iid == __uuidof(IAgileObject)) {
            *obj = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *obj = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&refs_);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        IUnknown* unk = nullptr;
        HRESULT activate_hr = E_FAIL;
        HRESULT hr = op->GetActivateResult(&activate_hr, &unk);
        if (SUCCEEDED(hr)) hr = activate_hr;
        if (SUCCEEDED(hr) && unk) {
            hr = unk->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&client_));
        }
        if (unk) unk->Release();
        hr_ = hr;
        SetEvent(done_);
        return S_OK;
    }

private:
    volatile LONG refs_ = 1;
    HANDLE done_ = nullptr;
    HRESULT hr_ = E_FAIL;
    IAudioClient* client_ = nullptr;
};

} // namespace

struct AppleMusicSource::MediaSessionState {
    media::GlobalSystemMediaTransportControlsSessionManager manager{nullptr};
    media::GlobalSystemMediaTransportControlsSession session{nullptr};
};

struct AppleMusicSource::MutedSession {
    DWORD pid = 0;
    bool previous_mute = false;
};

struct AppleMusicSource::MonitorBlock {
    WAVEHDR hdr{};
    std::vector<int16_t> data;
    bool prepared = false;
};

AppleMusicSource::AppleMusicSource(AppleMusicConfig cfg)
    : cfg_{cfg}, media_{std::make_unique<MediaSessionState>()} {}

AppleMusicSource::~AppleMusicSource() { shutdown(); }

bool AppleMusicSource::initialize() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    com_initialized_ = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        auth_ = AuthState::error;
        log::warn("[apple] CoInitializeEx failed: 0x{:08x}", static_cast<unsigned>(hr));
        return false;
    }
    auth_ = AuthState::none_required;
    return cfg_.enabled;
}

void AppleMusicSource::shutdown() noexcept {
    std::scoped_lock lk{mu_};
    close_capture_locked();
    state_.store(PlaybackState::stopped, std::memory_order_release);
    if (com_initialized_) {
        CoUninitialize();
        com_initialized_ = false;
    }
}

void AppleMusicSource::close_capture_locked() noexcept {
    if (audio_client_ && capture_started_) audio_client_->Stop();
    capture_started_ = false;
    close_monitor_locked();
    restore_external_mute_locked();
    release_com(capture_client_);
    release_com(audio_client_);
    release_com(device_);
    release_com(enumerator_);
    if (mix_format_) {
        CoTaskMemFree(mix_format_);
        mix_format_ = nullptr;
    }
    scratch_.clear();
    resample_accum_ = 0;
    mute_refresh_ticks_ = 0;
    using_device_capture_ = false;
}

bool AppleMusicSource::ensure_capture_locked() {
    if (capture_client_) return true;
    if (cfg_.capture_mode == "auto") {
        if (ensure_device_capture_locked()) return true;
        log::warn("[apple] auto capture could not open '{}'; falling back to process loopback",
                  cfg_.capture_device.empty() ? "CABLE Output" : cfg_.capture_device);
        close_capture_locked();
        return ensure_process_loopback_locked();
    }
    if (cfg_.capture_mode == "device") return ensure_device_capture_locked();
    return ensure_process_loopback_locked();
}

bool AppleMusicSource::device_capture_requested_locked() const noexcept {
    return cfg_.capture_mode == "device" || cfg_.capture_mode == "auto";
}

bool AppleMusicSource::ensure_process_loopback_locked() {
    if (capture_client_) return true;

    const uint32_t pid = GetCurrentProcessId();

    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activate_params{};
    activate_params.vt = VT_BLOB;
    activate_params.blob.cbSize = sizeof(params);
    activate_params.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto* handler = new AudioActivationHandler();
    IActivateAudioInterfaceAsyncOperation* op = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                             __uuidof(IAudioClient), &activate_params, handler,
                                             &op);
    if (FAILED(hr)) {
        handler->Release();
        log::warn("[apple] process-loopback exclusion activation failed for fh6 pid {}: 0x{:08x}",
                  pid,
                  static_cast<unsigned>(hr));
        auth_ = AuthState::error;
        return false;
    }

    hr = handler->wait(&audio_client_);
    if (op) op->Release();
    handler->Release();
    if (FAILED(hr) || !audio_client_) {
        log::warn("[apple] process-loopback exclusion completion failed for fh6 pid {}: 0x{:08x}",
                  pid,
                  static_cast<unsigned>(hr));
        auth_ = AuthState::error;
        close_capture_locked();
        return false;
    }

    hr = audio_client_->GetMixFormat(&mix_format_);
    if (FAILED(hr) || !mix_format_) {
        log::warn("[apple] GetMixFormat failed: 0x{:08x}; using 48kHz stereo PCM fallback",
                  static_cast<unsigned>(hr));
        mix_format_ = alloc_pcm_format(kOutRate);
        if (!mix_format_) {
            auth_ = AuthState::error;
            close_capture_locked();
            return false;
        }
    }

    constexpr REFERENCE_TIME buffer_duration = 1'000'000; // 100 ms
    hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                   buffer_duration, 0, mix_format_, nullptr);
    if (FAILED(hr) && mix_format_->nSamplesPerSec != 44100) {
        log::warn("[apple] loopback Initialize failed at 48kHz: 0x{:08x}; retrying 44.1kHz",
                  static_cast<unsigned>(hr));
        CoTaskMemFree(mix_format_);
        mix_format_ = alloc_pcm_format(44100);
        if (mix_format_) {
            hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                           buffer_duration, 0, mix_format_, nullptr);
        }
    }
    if (FAILED(hr)) {
        log::warn("[apple] loopback Initialize failed: 0x{:08x}", static_cast<unsigned>(hr));
        auth_ = AuthState::error;
        close_capture_locked();
        return false;
    }

    hr = audio_client_->GetService(__uuidof(IAudioCaptureClient),
                                   reinterpret_cast<void**>(&capture_client_));
    if (FAILED(hr)) {
        log::warn("[apple] IAudioCaptureClient unavailable: 0x{:08x}",
                  static_cast<unsigned>(hr));
        auth_ = AuthState::error;
        close_capture_locked();
        return false;
    }

    hr = audio_client_->Start();
    if (FAILED(hr)) {
        log::warn("[apple] loopback Start failed: 0x{:08x}", static_cast<unsigned>(hr));
        auth_ = AuthState::error;
        close_capture_locked();
        return false;
    }

    capture_started_ = true;
    using_device_capture_ = false;
    auth_ = AuthState::none_required;
    eq_.set_options(false, {}, static_cast<float>(kOutRate));
    log::info("[apple] loopback capture started excluding fh6 pid {} ({} Hz, {} ch, {} bits)",
              pid, mix_format_->nSamplesPerSec, mix_format_->nChannels,
              mix_format_->wBitsPerSample);
    return true;
}

bool AppleMusicSource::ensure_device_capture_locked() {
    if (capture_client_) return true;

    const auto requested = cfg_.capture_device.empty() ? std::string{"CABLE Output"}
                                                       : cfg_.capture_device;
    const auto requested_w = widen_ascii(requested);

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator_));
    if (FAILED(hr) || !enumerator_) {
        log::warn("[apple] device capture enumerator unavailable: 0x{:08x}",
                  static_cast<unsigned>(hr));
        auth_ = AuthState::error;
        return false;
    }

    IMMDeviceCollection* devices = nullptr;
    hr = enumerator_->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices);
    if (FAILED(hr) || !devices) {
        log::warn("[apple] capture device enumeration failed: 0x{:08x}",
                  static_cast<unsigned>(hr));
        auth_ = AuthState::error;
        close_capture_locked();
        return false;
    }

    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count && !device_; ++i) {
        IMMDevice* candidate = nullptr;
        if (FAILED(devices->Item(i, &candidate)) || !candidate) continue;
        const auto name = device_friendly_name(candidate);
        if (device_name_matches(name, requested_w)) {
            device_ = candidate;
            candidate = nullptr;
            log::info("[apple] selected capture device '{}'", winrt::to_string(name));
        }
        release_com(candidate);
    }
    devices->Release();

    if (!device_) {
        log::warn("[apple] capture device matching '{}' not found", requested);
        auth_ = AuthState::error;
        close_capture_locked();
        return false;
    }

    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(&audio_client_));
    if (FAILED(hr) || !audio_client_) {
        log::warn("[apple] capture device activation failed: 0x{:08x}",
                  static_cast<unsigned>(hr));
        auth_ = AuthState::error;
        close_capture_locked();
        return false;
    }

    hr = audio_client_->GetMixFormat(&mix_format_);
    if (FAILED(hr) || !mix_format_) {
        log::warn("[apple] capture device GetMixFormat failed: 0x{:08x}",
                  static_cast<unsigned>(hr));
        auth_ = AuthState::error;
        close_capture_locked();
        return false;
    }

    constexpr REFERENCE_TIME buffer_duration = 1'000'000; // 100 ms
    hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, buffer_duration, 0, mix_format_,
                                   nullptr);
    if (FAILED(hr)) {
        log::warn("[apple] capture device Initialize failed: 0x{:08x}",
                  static_cast<unsigned>(hr));
        auth_ = AuthState::error;
        close_capture_locked();
        return false;
    }

    hr = audio_client_->GetService(__uuidof(IAudioCaptureClient),
                                   reinterpret_cast<void**>(&capture_client_));
    if (FAILED(hr)) {
        log::warn("[apple] capture device IAudioCaptureClient unavailable: 0x{:08x}",
                  static_cast<unsigned>(hr));
        auth_ = AuthState::error;
        close_capture_locked();
        return false;
    }

    hr = audio_client_->Start();
    if (FAILED(hr)) {
        log::warn("[apple] capture device Start failed: 0x{:08x}", static_cast<unsigned>(hr));
        auth_ = AuthState::error;
        close_capture_locked();
        return false;
    }

    capture_started_ = true;
    using_device_capture_ = true;
    auth_ = AuthState::none_required;
    eq_.set_options(false, {}, static_cast<float>(kOutRate));
    log::info("[apple] capture device started ({} Hz, {} ch, {} bits)",
              mix_format_->nSamplesPerSec, mix_format_->nChannels, mix_format_->wBitsPerSample);
    return true;
}

bool AppleMusicSource::ensure_monitor_locked() noexcept {
    if (monitor_wave_out_) return true;

    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = kOutChannels;
    fmt.nSamplesPerSec = kOutRate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

    HWAVEOUT out = nullptr;
    MMRESULT rc = waveOutOpen(&out, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
    if (rc != MMSYSERR_NOERROR) {
        log::warn("[apple] monitor output open failed: {}", static_cast<unsigned>(rc));
        return false;
    }
    monitor_wave_out_ = out;
    log::info("[apple] monitoring cable capture to default Windows output");
    return true;
}

void AppleMusicSource::close_monitor_locked() noexcept {
    auto out = reinterpret_cast<HWAVEOUT>(monitor_wave_out_);
    if (!out) return;

    waveOutReset(out);
    for (auto& block : monitor_blocks_) {
        if (block && block->prepared) waveOutUnprepareHeader(out, &block->hdr, sizeof(WAVEHDR));
    }
    monitor_blocks_.clear();
    waveOutClose(out);
    monitor_wave_out_ = nullptr;
}

void AppleMusicSource::reclaim_monitor_blocks_locked() noexcept {
    auto out = reinterpret_cast<HWAVEOUT>(monitor_wave_out_);
    if (!out) return;
    auto it = monitor_blocks_.begin();
    while (it != monitor_blocks_.end()) {
        auto& block = *it;
        if (!block || (block->hdr.dwFlags & WHDR_DONE)) {
            if (block && block->prepared)
                waveOutUnprepareHeader(out, &block->hdr, sizeof(WAVEHDR));
            it = monitor_blocks_.erase(it);
        } else {
            ++it;
        }
    }
}

void AppleMusicSource::render_monitor_locked(const int16_t* samples, std::size_t frames) noexcept {
    if (!samples || frames == 0 || audio_sink_active_ || game_foreground_ ||
        !cfg_.monitor_when_radio_inactive || !using_device_capture_)
        return;
    if (!ensure_monitor_locked()) return;
    reclaim_monitor_blocks_locked();
    if (monitor_blocks_.size() >= 8) return;

    auto out = reinterpret_cast<HWAVEOUT>(monitor_wave_out_);
    auto block = std::make_unique<MonitorBlock>();
    block->data.assign(samples, samples + frames * kOutChannels);
    block->hdr.lpData = reinterpret_cast<LPSTR>(block->data.data());
    block->hdr.dwBufferLength = static_cast<DWORD>(block->data.size() * sizeof(int16_t));
    if (waveOutPrepareHeader(out, &block->hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) return;
    block->prepared = true;
    if (waveOutWrite(out, &block->hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
        waveOutUnprepareHeader(out, &block->hdr, sizeof(WAVEHDR));
        return;
    }
    monitor_blocks_.push_back(std::move(block));
}

void AppleMusicSource::send_media_key(uint16_t vk) const {
    if (!cfg_.transport_controls) return;
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = vk;
    inputs[1] = inputs[0];
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}

void AppleMusicSource::set_external_mute_locked(bool muted) noexcept {
    if (muted && !cfg_.mute_external_output) return;
    if (!muted) {
        restore_external_mute_locked();
        return;
    }

    auto pids = find_apple_music_pids();
    if (pids.empty()) return;

    IMMDeviceEnumerator* dev_enum = nullptr;
    IMMDevice* endpoint = nullptr;
    IAudioSessionManager2* manager = nullptr;
    IAudioSessionEnumerator* sessions = nullptr;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&dev_enum));
    if (FAILED(hr)) return;
    hr = dev_enum->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint);
    if (SUCCEEDED(hr)) {
        hr = endpoint->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&manager));
    }
    if (SUCCEEDED(hr)) hr = manager->GetSessionEnumerator(&sessions);

    int muted_count = 0;
    if (SUCCEEDED(hr) && sessions) {
        int count = 0;
        if (SUCCEEDED(sessions->GetCount(&count))) {
            for (int i = 0; i < count; ++i) {
                IAudioSessionControl* control = nullptr;
                IAudioSessionControl2* control2 = nullptr;
                ISimpleAudioVolume* volume = nullptr;
                DWORD pid = 0;
                auto cleanup_session = [&] {
                    release_com(volume);
                    release_com(control2);
                    release_com(control);
                };

                if (FAILED(sessions->GetSession(i, &control)) || !control) {
                    cleanup_session();
                    continue;
                }
                if (FAILED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                                                   reinterpret_cast<void**>(&control2))) ||
                    !control2) {
                    cleanup_session();
                    continue;
                }
                if (FAILED(control2->GetProcessId(&pid)) || !contains_pid(pids, pid)) {
                    cleanup_session();
                    continue;
                }
                if (FAILED(control->QueryInterface(__uuidof(ISimpleAudioVolume),
                                                   reinterpret_cast<void**>(&volume))) ||
                    !volume) {
                    cleanup_session();
                    continue;
                }

                BOOL was_muted = FALSE;
                volume->GetMute(&was_muted);
                const auto known = std::find_if(
                    muted_sessions_.begin(), muted_sessions_.end(),
                    [pid](const MutedSession& session) { return session.pid == pid; });
                if (known == muted_sessions_.end()) {
                    muted_sessions_.push_back(MutedSession{pid, was_muted != FALSE});
                }
                if (SUCCEEDED(volume->SetMute(TRUE, nullptr))) ++muted_count;
                cleanup_session();
            }
        }
    }

    release_com(sessions);
    release_com(manager);
    release_com(endpoint);
    release_com(dev_enum);

    if (muted_count > 0) log::info("[apple] muted {} Apple Music output session(s)", muted_count);
}

void AppleMusicSource::restore_external_mute_locked() noexcept {
    if (muted_sessions_.empty()) return;

    IMMDeviceEnumerator* dev_enum = nullptr;
    IMMDevice* endpoint = nullptr;
    IAudioSessionManager2* manager = nullptr;
    IAudioSessionEnumerator* sessions = nullptr;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&dev_enum));
    if (FAILED(hr)) {
        muted_sessions_.clear();
        return;
    }
    hr = dev_enum->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint);
    if (SUCCEEDED(hr)) {
        hr = endpoint->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&manager));
    }
    if (SUCCEEDED(hr)) hr = manager->GetSessionEnumerator(&sessions);

    int restored_count = 0;
    if (SUCCEEDED(hr) && sessions) {
        int count = 0;
        if (SUCCEEDED(sessions->GetCount(&count))) {
            for (int i = 0; i < count; ++i) {
                IAudioSessionControl* control = nullptr;
                IAudioSessionControl2* control2 = nullptr;
                ISimpleAudioVolume* volume = nullptr;
                DWORD pid = 0;
                auto cleanup_session = [&] {
                    release_com(volume);
                    release_com(control2);
                    release_com(control);
                };

                if (FAILED(sessions->GetSession(i, &control)) || !control) {
                    cleanup_session();
                    continue;
                }
                if (FAILED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                                                   reinterpret_cast<void**>(&control2))) ||
                    !control2) {
                    cleanup_session();
                    continue;
                }
                if (FAILED(control2->GetProcessId(&pid))) {
                    cleanup_session();
                    continue;
                }
                {
                    const auto known = std::find_if(
                        muted_sessions_.begin(), muted_sessions_.end(),
                        [pid](const MutedSession& session) { return session.pid == pid; });
                    if (known == muted_sessions_.end()) {
                        cleanup_session();
                        continue;
                    }
                    if (FAILED(control->QueryInterface(__uuidof(ISimpleAudioVolume),
                                                       reinterpret_cast<void**>(&volume))) ||
                        !volume) {
                        cleanup_session();
                        continue;
                    }
                    if (SUCCEEDED(volume->SetMute(known->previous_mute ? TRUE : FALSE, nullptr)))
                        ++restored_count;
                }
                cleanup_session();
            }
        }
    }

    muted_sessions_.clear();
    release_com(sessions);
    release_com(manager);
    release_com(endpoint);
    release_com(dev_enum);

    if (restored_count > 0)
        log::info("[apple] restored {} Apple Music output session(s)", restored_count);
}

bool AppleMusicSource::refresh_media_session_locked(bool force) {
    if (!media_) media_ = std::make_unique<MediaSessionState>();
    if (media_->session && !force) return true;
    try {
        if (!media_->manager) {
            media_->manager =
                media::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        }
        if (!media_->manager) return false;
        media::GlobalSystemMediaTransportControlsSession fallback{nullptr};
        media::GlobalSystemMediaTransportControlsSession apple{nullptr};

        fallback = media_->manager.GetCurrentSession();
        if (fallback && is_apple_music_session_id(narrow(fallback.SourceAppUserModelId()))) {
            apple = fallback;
        } else {
            for (const auto& session : media_->manager.GetSessions()) {
                if (!session) continue;
                if (is_apple_music_session_id(narrow(session.SourceAppUserModelId()))) {
                    apple = session;
                    break;
                }
            }
        }
        media_->session = apple ? apple : fallback;
        return media_->session != nullptr;
    } catch (const winrt::hresult_error& e) {
        log::warn("[apple] media session unavailable: 0x{:08x}",
                  static_cast<unsigned>(e.code()));
    } catch (...) {
        log::warn("[apple] media session unavailable");
    }
    return false;
}

bool AppleMusicSource::send_session_command_locked(MediaCommand cmd) {
    if (!cfg_.transport_controls) return false;
    if (!refresh_media_session_locked(true)) return false;
    try {
        bool ok = false;
        switch (cmd) {
            case MediaCommand::play:
                ok = media_->session.TryPlayAsync().get();
                break;
            case MediaCommand::pause:
                ok = media_->session.TryPauseAsync().get();
                break;
            case MediaCommand::next:
                ok = media_->session.TrySkipNextAsync().get();
                break;
            case MediaCommand::previous:
                ok = media_->session.TrySkipPreviousAsync().get();
                break;
        }
        if (!ok) media_->session = nullptr;
        return ok;
    } catch (...) {
        media_->session = nullptr;
        return false;
    }
}

void AppleMusicSource::play() {
    std::scoped_lock lk{mu_};
    if (!ensure_capture_locked()) {
        state_.store(PlaybackState::stopped, std::memory_order_release);
        return;
    }
    set_external_mute_locked(true);
    paused_by_radio_ = false;
    state_.store(PlaybackState::playing, std::memory_order_release);
    if (radio_active_) {
        if (!send_session_command_locked(MediaCommand::play)) send_media_key(VK_MEDIA_PLAY_PAUSE);
    } else {
        paused_by_radio_ = true;
    }
}

void AppleMusicSource::pause() {
    {
        std::scoped_lock lk{mu_};
        if (!send_session_command_locked(MediaCommand::pause)) send_media_key(VK_MEDIA_PLAY_PAUSE);
    }
    state_.store(PlaybackState::paused, std::memory_order_release);
}

void AppleMusicSource::stop() {
    std::scoped_lock lk{mu_};
    paused_by_radio_ = false;
    close_capture_locked();
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

void AppleMusicSource::next() {
    std::scoped_lock lk{mu_};
    if (!send_session_command_locked(MediaCommand::next)) send_media_key(VK_MEDIA_NEXT_TRACK);
    position_ms_.store(0, std::memory_order_release);
    resample_accum_ = 0;
    drain_requested_ = true;
}

void AppleMusicSource::previous() {
    std::scoped_lock lk{mu_};
    if (!send_session_command_locked(MediaCommand::previous)) send_media_key(VK_MEDIA_PREV_TRACK);
    position_ms_.store(0, std::memory_order_release);
    resample_accum_ = 0;
    drain_requested_ = true;
}

void AppleMusicSource::flush_capture_packets_locked() noexcept {
    if (!capture_client_) return;
    for (;;) {
        UINT32 packet_frames = 0;
        if (FAILED(capture_client_->GetNextPacketSize(&packet_frames)) || packet_frames == 0) break;
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(capture_client_->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
        capture_client_->ReleaseBuffer(frames);
    }
    resample_accum_ = 0;
}

void AppleMusicSource::append_frames(const void* data, uint32_t frames, uint32_t flags,
                                     RingBuffer& ring) {
    if (!mix_format_ || frames == 0) return;

    const auto in_rate = std::max<uint32_t>(1, mix_format_->nSamplesPerSec);
    const auto in_channels = std::max<uint16_t>(1, mix_format_->nChannels);
    const auto block_align = std::max<uint16_t>(1, mix_format_->nBlockAlign);
    const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
    if (silent && std::chrono::steady_clock::now() < ignore_silent_capture_until_) return;

    scratch_.clear();
    scratch_.reserve(size_t{frames} * kOutChannels);

    const auto* bytes = static_cast<const std::byte*>(data);
    for (uint32_t i = 0; i < frames; ++i) {
        float l = 0.0f;
        float r = 0.0f;
        if (!silent && bytes) {
            const auto* frame = bytes + size_t{i} * block_align;
            l = sample_as_float(frame, 0, *mix_format_);
            r = in_channels > 1 ? sample_as_float(frame, 1, *mix_format_) : l;
        }

        resample_accum_ += kOutRate;
        while (resample_accum_ >= in_rate) {
            scratch_.push_back(clamp_s16(l));
            scratch_.push_back(clamp_s16(r));
            resample_accum_ -= in_rate;
        }
    }

    if (scratch_.empty()) return;
    const auto out_frames = scratch_.size() / kOutChannels;
    eq_.process(scratch_.data(), out_frames);
    render_monitor_locked(scratch_.data(), out_frames);
    if (!audio_sink_active_) return;

    const auto bytes_total = out_frames * kFrameBytes;
    if (ring.write(scratch_.data(), bytes_total) == bytes_total) {
        const auto previous = position_ms_.load(std::memory_order_acquire);
        position_ms_.store(previous + (bytes_total * 1000ull / kPcmBytesPerSec),
                           std::memory_order_release);
    }
}

void AppleMusicSource::pump(RingBuffer& ring) {
    if (state_.load(std::memory_order_acquire) != PlaybackState::playing) return;

    std::scoped_lock lk{mu_};
    if (!ensure_capture_locked()) return;
    if (++mute_refresh_ticks_ >= 50) {
        mute_refresh_ticks_ = 0;
        set_external_mute_locked(true);
    }
    if (++metadata_refresh_ticks_ >= 25) {
        metadata_refresh_ticks_ = 0;
        refresh_track_locked(false);
    }

    UINT32 packet_frames = 0;
    HRESULT hr = capture_client_->GetNextPacketSize(&packet_frames);
    if (FAILED(hr)) {
        log::warn("[apple] GetNextPacketSize failed: 0x{:08x}", static_cast<unsigned>(hr));
        close_capture_locked();
        state_.store(PlaybackState::stopped, std::memory_order_release);
        return;
    }

    while (packet_frames > 0 && ring.writable() >= kFrameBytes) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        hr = capture_client_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(hr)) break;
        append_frames(data, frames, flags, ring);
        capture_client_->ReleaseBuffer(frames);
        hr = capture_client_->GetNextPacketSize(&packet_frames);
        if (FAILED(hr)) break;
    }
}

void AppleMusicSource::refresh_track_locked(bool force) {
    TrackInfo next;
    next.title = "Apple Music";
    next.position_ms = position_ms_.load(std::memory_order_acquire);

    try {
        if (refresh_media_session_locked(force)) {
            auto props = media_->session.TryGetMediaPropertiesAsync().get();
            auto title = narrow(props.Title());
            auto artist = narrow(props.Artist());
            auto album = narrow(props.AlbumTitle());
            normalize_artist_album(artist, album, show_album_in_hud_);
            if (!title.empty()) next.title = std::move(title);
            if (!artist.empty()) next.artist = std::move(artist);
            if (!album.empty()) next.album = std::move(album);

            try {
                const auto timeline = media_->session.GetTimelineProperties();
                const auto position = ms_from_timespan(timeline.Position());
                const auto start = ms_from_timespan(timeline.StartTime());
                const auto end = ms_from_timespan(timeline.EndTime());
                if (position > 0) {
                    next.position_ms = position;
                    position_ms_.store(position, std::memory_order_release);
                }
                if (end > start) next.duration_ms = end - start;
            } catch (...) {}
        }
    } catch (...) {}

    if (next.artist.empty()) next.artist = "Apple Music";

    auto key = next.title + "\n" + next.artist + "\n" + next.album;
    refresh_artwork_locked(key);
    if (!key.empty() && !last_track_key_.empty() && key != last_track_key_) {
        position_ms_.store(0, std::memory_order_release);
        next.position_ms = 0;
        resample_accum_ = 0;
        drain_requested_ = true;
        log::info(R"([apple] track changed "{}" / "{}")", next.title, next.artist);
    }
    if (!key.empty()) last_track_key_ = std::move(key);
    cached_track_ = std::move(next);
}

void AppleMusicSource::refresh_artwork_locked(const std::string& key) {
    if (key.empty() || key == cached_art_key_) return;
    cached_art_key_ = key;
    cached_art_ = {};

    try {
        if (!media_ || !media_->session) return;
        const auto props = media_->session.TryGetMediaPropertiesAsync().get();
        const auto ref = props.Thumbnail();
        if (!ref) return;

        const streams::IRandomAccessStreamWithContentType stream = ref.OpenReadAsync().get();
        const uint64_t size = stream ? stream.Size() : 0;
        if (size == 0 || size > (8ull << 20)) return;

        streams::DataReader reader{stream};
        reader.LoadAsync(static_cast<uint32_t>(size)).get();

        ArtworkImage art;
        art.bytes.resize(static_cast<std::size_t>(size));
        reader.ReadBytes(winrt::array_view<uint8_t>(reinterpret_cast<uint8_t*>(art.bytes.data()),
                                                    reinterpret_cast<uint8_t*>(art.bytes.data()) +
                                                        art.bytes.size()));
        art.mime = narrow(stream.ContentType());
        if (art.mime.empty()) art.mime = "image/jpeg";
        cached_art_ = std::move(art);
    } catch (...) {
        cached_art_ = {};
    }
}

void AppleMusicSource::set_config(AppleMusicConfig cfg) {
    std::scoped_lock lk{mu_};
    const bool capture_changed = cfg.capture_mode != cfg_.capture_mode ||
                                 cfg.capture_device != cfg_.capture_device;
    cfg_ = cfg;
    if (!cfg_.mute_external_output) restore_external_mute_locked();
    if (capture_changed) close_capture_locked();
}

void AppleMusicSource::set_playback_options(const PlaybackConfig& opts) {
    std::scoped_lock lk{mu_};
    if (show_album_in_hud_ != opts.show_album_in_hud) {
        show_album_in_hud_ = opts.show_album_in_hud;
        last_track_key_.clear();
        metadata_refresh_ticks_ = 25;
    }
    eq_.set_options(opts.equalizer_enabled, opts.equalizer_bands, static_cast<float>(kOutRate));
}

bool AppleMusicSource::consume_drain_request() noexcept {
    std::scoped_lock lk{mu_};
    const bool out = drain_requested_;
    drain_requested_ = false;
    return out;
}

void AppleMusicSource::on_radio_active_changed(bool active) {
    std::scoped_lock lk{mu_};
    radio_active_ = active;
    if (active) close_monitor_locked();
    if (!active && game_foreground_) close_monitor_locked();

    if (active) {
        if (paused_by_radio_) {
            if (ensure_capture_locked()) {
                set_external_mute_locked(true);
                if (!send_session_command_locked(MediaCommand::play))
                    send_media_key(VK_MEDIA_PLAY_PAUSE);
                state_.store(PlaybackState::playing, std::memory_order_release);
            }
            paused_by_radio_ = false;
        }
        return;
    }

    if (state_.load(std::memory_order_acquire) == PlaybackState::playing) {
        if (!send_session_command_locked(MediaCommand::pause)) send_media_key(VK_MEDIA_PLAY_PAUSE);
        state_.store(PlaybackState::paused, std::memory_order_release);
        paused_by_radio_ = true;
    }
}

void AppleMusicSource::on_audio_sink_active_changed(bool active) {
    std::scoped_lock lk{mu_};
    audio_sink_active_ = active;
    ignore_silent_capture_until_ = std::chrono::steady_clock::now() + 750ms;
    flush_capture_packets_locked();
    if (active) {
        close_monitor_locked();
        return;
    }
    if (state_.load(std::memory_order_acquire) == PlaybackState::playing) {
        if (!send_session_command_locked(MediaCommand::pause)) send_media_key(VK_MEDIA_PLAY_PAUSE);
        state_.store(PlaybackState::paused, std::memory_order_release);
        paused_by_radio_ = true;
    }
}

void AppleMusicSource::on_game_foreground_changed(bool foreground) {
    std::scoped_lock lk{mu_};
    game_foreground_ = foreground;
    if (foreground) close_monitor_locked();
    if (!foreground && !radio_active_ && cfg_.monitor_when_radio_inactive &&
        device_capture_requested_locked()) {
        if (ensure_capture_locked()) {
            if (!send_session_command_locked(MediaCommand::play)) send_media_key(VK_MEDIA_PLAY_PAUSE);
            state_.store(PlaybackState::playing, std::memory_order_release);
            paused_by_radio_ = false;
        }
    }
}

TrackInfo AppleMusicSource::current_track() const {
    std::scoped_lock lk{mu_};
    TrackInfo out = cached_track_;
    if (out.title.empty()) out.title = "Apple Music";
    if (out.artist.empty()) out.artist = "Apple Music";
    out.position_ms = position_ms_.load(std::memory_order_acquire);
    if (!cached_art_.bytes.empty() && !cached_art_key_.empty()) {
        out.artwork_url = "/api/artwork?v=" +
                          std::to_string(std::hash<std::string>{}(cached_art_key_));
    }
    return out;
}

std::optional<ArtworkImage> AppleMusicSource::artwork() const {
    std::scoped_lock lk{mu_};
    if (!cached_art_.bytes.empty()) return cached_art_;
    return std::nullopt;
}

std::string AppleMusicSource::auth_instructions() const {
    return "Start playback in the Apple Music app or music.apple.com, then switch to this source. "
           "The mod captures the authorized Windows output device, so track metadata and seek are "
           "not available from Apple Music's protected stream.";
}

} // namespace fh6::sources
