#include "fh6/sources/external_audio_source.hpp"
#include "fh6/sources/external_media_session.hpp"

#include "fh6/log.hpp"
#include "fh6/ring_buffer.hpp"

#include <windows.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <propsys.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <limits>
#include <string_view>

namespace fh6::sources {
namespace {

constexpr uint32_t kTargetSampleRate = 48000;
constexpr uint32_t kTargetChannels = 2;
constexpr std::size_t kMaxQueuedSamples = kTargetSampleRate * kTargetChannels * 2;

// Upper bound on audio in flight (ring + queue) so captured audio stays close
// to real time and the live media-session metadata lines up with what's heard.
// 250 ms at 48 kHz stereo.
constexpr std::size_t kMaxInFlightSamples = kTargetSampleRate * kTargetChannels / 4;

template <class T> class ComPtr {
public:
 ComPtr() = default;
 ComPtr(const ComPtr&) = delete;
 ComPtr& operator=(const ComPtr&) = delete;
 ComPtr(ComPtr&& other) noexcept : p_{other.p_} { other.p_ = nullptr; }
 ComPtr& operator=(ComPtr&& other) noexcept {
  if (this != &other) {
   reset();
   p_ = other.p_;
   other.p_ = nullptr;
  }
  return *this;
 }
 ~ComPtr() { reset(); }

 T* get() const noexcept { return p_; }
 T** put() noexcept {
  reset();
  return &p_;
 }
 T* operator->() const noexcept { return p_; }
 explicit operator bool() const noexcept { return p_ != nullptr; }

 void reset(T* p = nullptr) noexcept {
  if (p_) p_->Release();
  p_ = p;
 }

private:
 T* p_ = nullptr;
};

struct CoTaskMemWaveFormat {
 WAVEFORMATEX* p = nullptr;
 ~CoTaskMemWaveFormat() {
  if (p) CoTaskMemFree(p);
 }
 WAVEFORMATEX** put() {
  if (p) {
   CoTaskMemFree(p);
   p = nullptr;
  }
  return &p;
 }
};

struct CoTaskMemString {
 wchar_t* p = nullptr;
 ~CoTaskMemString() {
  if (p) CoTaskMemFree(p);
 }
 wchar_t** put() {
  if (p) {
   CoTaskMemFree(p);
   p = nullptr;
  }
  return &p;
 }
};

std::string hresult_string(const char* where, HRESULT hr) {
 char buf[160];
 std::snprintf(buf, sizeof(buf), "%s failed: HRESULT 0x%08lX", where,
  static_cast<unsigned long>(static_cast<uint32_t>(hr)));
 return buf;
}

std::wstring utf8_to_wide(std::string_view s) {
 if (s.empty()) return {};
 int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
 if (n <= 0) return {};
 std::wstring out(static_cast<std::size_t>(n), L'\0');
 MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
 return out;
}

std::string wide_to_utf8(const wchar_t* s) {
 if (!s || !*s) return {};
 int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
 if (n <= 1) return {};
 std::string out(static_cast<std::size_t>(n - 1), '\0');
 WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
 return out;
}

std::wstring lower_copy(std::wstring s) {
 std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
  return static_cast<wchar_t>(std::towlower(c));
 });
 return s;
}

std::string friendly_name(IMMDevice* device) {
 if (!device) return {};
 ComPtr<IPropertyStore> props;
 HRESULT hr = device->OpenPropertyStore(STGM_READ, props.put());
 if (FAILED(hr) || !props) return {};

 PROPVARIANT v;
 PropVariantInit(&v);
 std::string out;
 if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
  out = wide_to_utf8(v.pwszVal);
 }
 PropVariantClear(&v);
 return out;
}

std::string endpoint_id(IMMDevice* device) {
 if (!device) return {};
 CoTaskMemString id;
 if (FAILED(device->GetId(id.put())) || !id.p) return {};
 return wide_to_utf8(id.p);
}

float clamp_sample(float v) noexcept {
 if (!std::isfinite(v)) return 0.0f;
 return std::clamp(v, -1.0f, 1.0f);
}

int16_t float_to_s16(float v) noexcept {
 v = clamp_sample(v);
 if (v >= 0.9999695f) return std::numeric_limits<int16_t>::max();
 if (v <= -1.0f) return std::numeric_limits<int16_t>::min();
 return static_cast<int16_t>(std::lrintf(v * 32767.0f));
}

int32_t read_i24_le(const BYTE* p) noexcept {
 int32_t v = static_cast<int32_t>(p[0]) |
  (static_cast<int32_t>(p[1]) << 8) |
  (static_cast<int32_t>(p[2]) << 16);
 if (v & 0x00800000) v |= 0xFF000000;
 return v;
}

float sample_to_float(const BYTE* p, WORD container_bits, WORD valid_bits, bool is_float) noexcept {
 if (is_float) {
  if (container_bits == 32) {
   float v;
   std::memcpy(&v, p, sizeof(v));
   return clamp_sample(v);
  }
  if (container_bits == 64) {
   double v;
   std::memcpy(&v, p, sizeof(v));
   return clamp_sample(static_cast<float>(v));
  }
  return 0.0f;
 }

 if (valid_bits == 0 || valid_bits > container_bits) valid_bits = container_bits;
 if (valid_bits == 0 || valid_bits > 32) return 0.0f;

 if (container_bits == 8) {
  const int32_t signed_value = static_cast<int32_t>(*p) - 128;
  return clamp_sample(static_cast<float>(signed_value) / 128.0f);
 }

 int32_t signed_value = 0;
 switch (container_bits) {
 case 16: {
  int16_t v;
  std::memcpy(&v, p, sizeof(v));
  signed_value = v;
  break;
 }
 case 24:
  signed_value = read_i24_le(p);
  break;
 case 32:
  std::memcpy(&signed_value, p, sizeof(signed_value));
  break;
 default:
  return 0.0f;
 }

 if (valid_bits < container_bits) {
  signed_value >>= static_cast<int>(container_bits - valid_bits);
 }

 const double scale = static_cast<double>(int64_t{1} << (valid_bits - 1u));
 return clamp_sample(static_cast<float>(static_cast<double>(signed_value) / scale));
}

struct InputFormat {
 uint32_t sample_rate = 0;
 uint16_t channels = 0;
 uint16_t container_bits = 0;
 uint16_t valid_bits = 0;
 uint16_t block_align = 0;
 bool is_float = false;
};

bool parse_input_format(const WAVEFORMATEX& fmt, InputFormat& out, std::string& error) {
 out.sample_rate = fmt.nSamplesPerSec;
 out.channels = fmt.nChannels;
 out.container_bits = fmt.wBitsPerSample;
 out.valid_bits = fmt.wBitsPerSample;
 out.block_align = fmt.nBlockAlign;

 if (out.sample_rate == 0 || out.channels == 0 || out.block_align == 0 || out.container_bits == 0) {
  error = "Invalid WASAPI mix format";
  return false;
 }

 if (fmt.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
  out.is_float = true;
  return true;
 }

 if (fmt.wFormatTag == WAVE_FORMAT_PCM) {
  out.is_float = false;
  return true;
 }

 if (fmt.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
  fmt.cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) {
  const auto& ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(fmt);
  out.container_bits = ext.Format.wBitsPerSample;
  out.valid_bits = ext.Samples.wValidBitsPerSample ? ext.Samples.wValidBitsPerSample : out.container_bits;
  out.block_align = ext.Format.nBlockAlign;
  if (out.valid_bits > out.container_bits) out.valid_bits = out.container_bits;
  if (IsEqualGUID(ext.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
   out.is_float = true;
   return true;
  }
  if (IsEqualGUID(ext.SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
   out.is_float = false;
   return true;
  }
 }

 error = "Unsupported WASAPI mix format";
 return false;
}

struct Resampler {
 InputFormat fmt;
 bool have_prev = false;
 float prev_l = 0.0f;
 float prev_r = 0.0f;
 double pos = 0.0;
 std::vector<float> in;
 std::vector<int16_t> out;

 void reset() noexcept {
  have_prev = false;
  prev_l = 0.0f;
  prev_r = 0.0f;
  pos = 0.0;
 }

 void push_packet(const BYTE* data, UINT32 frame_count, DWORD flags) {
  if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) reset();

  in.clear();
  in.reserve((static_cast<std::size_t>(frame_count) + (have_prev ? 1u : 0u)) * 2u);

  if (have_prev) {
   in.push_back(prev_l);
   in.push_back(prev_r);
  }

  const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
  const uint16_t bytes_per_sample = static_cast<uint16_t>((fmt.container_bits + 7u) / 8u);

  for (UINT32 f = 0; f < frame_count; ++f) {
   float l = 0.0f;
   float r = 0.0f;

   if (!silent && data) {
    const BYTE* frame = data + static_cast<std::size_t>(f) * fmt.block_align;
    if (fmt.channels == 1) {
     l = sample_to_float(frame, fmt.container_bits, fmt.valid_bits, fmt.is_float);
     r = l;
    } else if (fmt.channels == 2) {
     l = sample_to_float(frame, fmt.container_bits, fmt.valid_bits, fmt.is_float);
     r = sample_to_float(frame + bytes_per_sample, fmt.container_bits, fmt.valid_bits, fmt.is_float);
    } else {
     double lsum = 0.0;
     double rsum = 0.0;
     double lw = 0.0;
     double rw = 0.0;
     for (uint16_t ch = 0; ch < fmt.channels; ++ch) {
      const float v = sample_to_float(frame + static_cast<std::size_t>(ch) * bytes_per_sample,
       fmt.container_bits, fmt.valid_bits, fmt.is_float);
      if (ch == 0) {
       lsum += v;
       lw += 1.0;
      } else if (ch == 1) {
       rsum += v;
       rw += 1.0;
      } else {
       lsum += v;
       rsum += v;
       lw += 1.0;
       rw += 1.0;
      }
     }
     l = static_cast<float>(lsum / std::max(1.0, lw));
     r = static_cast<float>(rsum / std::max(1.0, rw));
    }
   }

   in.push_back(l);
   in.push_back(r);
  }

  const std::size_t frames = in.size() / 2u;
  if (frames == 0) return;

  prev_l = in[(frames - 1u) * 2u + 0u];
  prev_r = in[(frames - 1u) * 2u + 1u];
  have_prev = true;

  if (frames < 2) return;

  const double step = static_cast<double>(fmt.sample_rate) / static_cast<double>(kTargetSampleRate);

  while (pos + 1.0 < static_cast<double>(frames)) {
   const auto i = static_cast<std::size_t>(pos);
   const float a = static_cast<float>(pos - static_cast<double>(i));

   const float l0 = in[i * 2u + 0u];
   const float r0 = in[i * 2u + 1u];
   const float l1 = in[(i + 1u) * 2u + 0u];
   const float r1 = in[(i + 1u) * 2u + 1u];

   out.push_back(float_to_s16(l0 + (l1 - l0) * a));
   out.push_back(float_to_s16(r0 + (r1 - r0) * a));

   pos += step;
  }

  pos -= static_cast<double>(frames - 1u);
 }
};

HRESULT get_configured_endpoint(IMMDeviceEnumerator* enumerator, const std::string& endpoint,
 IMMDevice** out_device) {
 if (endpoint.empty()) {
  return enumerator->GetDefaultAudioEndpoint(eRender, eConsole, out_device);
 }

 const auto wanted = utf8_to_wide(endpoint);
 HRESULT hr = enumerator->GetDevice(wanted.c_str(), out_device);
 if (SUCCEEDED(hr)) return hr;

 ComPtr<IMMDeviceCollection> devices;
 hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.put());
 if (FAILED(hr) || !devices) return hr;

 UINT count = 0;
 if (FAILED(devices->GetCount(&count))) return E_NOTFOUND;

 const auto wanted_l = lower_copy(wanted);
 for (UINT i = 0; i < count; ++i) {
  ComPtr<IMMDevice> dev;
  if (FAILED(devices->Item(i, dev.put())) || !dev) continue;

  const auto name_l = lower_copy(utf8_to_wide(friendly_name(dev.get())));
  const auto id_l = lower_copy(utf8_to_wide(endpoint_id(dev.get())));
  if ((!name_l.empty() && name_l.find(wanted_l) != std::wstring::npos) ||
   (!id_l.empty() && id_l.find(wanted_l) != std::wstring::npos)) {
   *out_device = dev.get();
   (*out_device)->AddRef();
   return S_OK;
  }
 }

 return E_NOTFOUND;
}

} // namespace

std::vector<ExternalAudioDevice> enumerate_external_audio_devices() {
 std::vector<ExternalAudioDevice> out;

 HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
 const bool co_initialized = SUCCEEDED(hr);
 if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return out;

 auto co_uninit = [&] {
  if (co_initialized) CoUninitialize();
 };

 ComPtr<IMMDeviceEnumerator> enumerator;
 hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
  IID_PPV_ARGS(enumerator.put()));
 if (FAILED(hr) || !enumerator) {
  co_uninit();
  return out;
 }

 std::string default_id;
 {
  ComPtr<IMMDevice> def;
  if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, def.put())) && def) {
   default_id = endpoint_id(def.get());
  }
 }

 ComPtr<IMMDeviceCollection> devices;
 hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.put());
 if (FAILED(hr) || !devices) {
  co_uninit();
  return out;
 }

 UINT count = 0;
 if (FAILED(devices->GetCount(&count))) {
  co_uninit();
  return out;
 }

 out.reserve(count);
 for (UINT i = 0; i < count; ++i) {
  ComPtr<IMMDevice> dev;
  if (FAILED(devices->Item(i, dev.put())) || !dev) continue;

  ExternalAudioDevice d;
  d.id = endpoint_id(dev.get());
  d.name = friendly_name(dev.get());
  if (d.name.empty()) d.name = d.id.empty() ? "Unknown playback device" : d.id;
  d.is_default = !default_id.empty() && d.id == default_id;
  if (!d.id.empty()) out.push_back(std::move(d));
 }

 std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
  if (a.is_default != b.is_default) return a.is_default && !b.is_default;
  return a.name < b.name;
 });

 co_uninit();
 return out;
}

void ExternalAudioSource::set_config(ExternalAudioConfig cfg) {
 bool endpoint_changed;
 {
  std::scoped_lock lk{meta_mu_};
  endpoint_changed = endpoint_id_ != cfg.endpoint_id;
  endpoint_id_ = std::move(cfg.endpoint_id);
  media_session_id_ = std::move(cfg.media_session_id);
 }

 // Only the capture endpoint needs a worker restart; the media session is read
 // live by current_track()/next()/previous(), so a change there costs nothing.
 if (endpoint_changed && state_.load(std::memory_order_acquire) == PlaybackState::playing) {
  stop_worker();
  clear_queue();
  start_worker();
 }
}

std::string ExternalAudioSource::configured_endpoint() const {
 std::scoped_lock lk{meta_mu_};
 return endpoint_id_;
}

std::string ExternalAudioSource::configured_media_session() const {
 std::scoped_lock lk{meta_mu_};
 return media_session_id_;
}

ExternalAudioSource::~ExternalAudioSource() {
 shutdown();
}

bool ExternalAudioSource::initialize() {
 return true;
}

void ExternalAudioSource::shutdown() noexcept {
 stop_worker();
 clear_queue();
 state_.store(PlaybackState::stopped, std::memory_order_release);
}

void ExternalAudioSource::play() {
 state_.store(PlaybackState::playing, std::memory_order_release);
 start_worker();
 set_media_transport(true);
}

void ExternalAudioSource::pause() {
 state_.store(PlaybackState::paused, std::memory_order_release);
 set_media_transport(false);
 stop_worker();
 clear_queue();
}

void ExternalAudioSource::stop() {
 state_.store(PlaybackState::stopped, std::memory_order_release);
 set_media_transport(false);
 stop_worker();
 clear_queue();
 position_ms_.store(0, std::memory_order_release);
}

// The game drains our station only while it's audible; when it stops (pause
// menu, radio off) the control loop reports it here so the live external player
// pauses in step instead of running on, then resumes when audio flows again.
void ExternalAudioSource::on_radio_audible(bool audible) {
 if (state_.load(std::memory_order_acquire) != PlaybackState::playing) return;
 set_media_transport(audible);
}

void ExternalAudioSource::set_media_transport(bool play) {
 if (media_active_.exchange(play) == play) return;
 const auto session = configured_media_session();
 if (play) external_audio_media_session_play(session);
 else      external_audio_media_session_pause(session);
}

void ExternalAudioSource::next() {
 (void)skip_next();
}

void ExternalAudioSource::previous() {
 (void)external_audio_media_session_previous(configured_media_session());
}

bool ExternalAudioSource::skip_next() {
 return external_audio_media_session_next(configured_media_session());
}

void ExternalAudioSource::pump(RingBuffer& ring) {
 if (state_.load(std::memory_order_acquire) != PlaybackState::playing) return;

 std::vector<int16_t> chunk;
 {
  std::scoped_lock lk{queue_mu_};
  std::size_t available = pcm_queue_.size() - queue_offset_;
  if (available == 0) return;

  // Real-time capture must not accumulate latency: the media-session metadata
  // is read live, so any audio backlog makes the next track show that far
  // ahead of when it's heard. Cap what's in flight (ring + queue) and skip the
  // oldest captured samples past it rather than letting the ring fill deep.
  const std::size_t backlog = ring.readable() / sizeof(int16_t);
  if (backlog + available > kMaxInFlightSamples) {
   const std::size_t drop =
    std::min(backlog + available - kMaxInFlightSamples, available) & ~std::size_t{1};
   queue_offset_ += drop;
   available -= drop;
  }

  std::size_t writable_samples = ring.writable() / sizeof(int16_t);
  writable_samples &= ~std::size_t{1};
  const std::size_t to_write = std::min(available, writable_samples);
  if (to_write > 0) {
   chunk.assign(pcm_queue_.begin() + static_cast<std::ptrdiff_t>(queue_offset_),
    pcm_queue_.begin() + static_cast<std::ptrdiff_t>(queue_offset_ + to_write));
   queue_offset_ += to_write;
  }
  compact_queue_locked();
 }

 if (!chunk.empty()) {
  const std::size_t bytes = chunk.size() * sizeof(int16_t);
  if (ring.write(chunk.data(), bytes) == bytes) {
   const auto frames = static_cast<uint64_t>(chunk.size() / kTargetChannels);
   position_ms_.fetch_add((frames * 1000u) / kTargetSampleRate, std::memory_order_acq_rel);
  }
 }
}

TrackInfo ExternalAudioSource::current_track() const {
 const auto fallback_position = position_ms_.load(std::memory_order_acquire);
 const auto selected_session = configured_media_session();
 if (auto t = external_audio_media_session_track(selected_session, fallback_position)) {
  if (t->album.empty()) t->album = "External Audio";
  return *t;
 }

 std::scoped_lock lk{meta_mu_};
 TrackInfo t;
 t.title = "External Audio";
 t.artist = device_name_.empty() ? "No media session metadata" : device_name_;
 t.album = "External Audio";
 t.position_ms = fallback_position;
 return t;
}

AuthState ExternalAudioSource::auth_state() const noexcept {
 std::scoped_lock lk{meta_mu_};
 return last_error_.empty() ? AuthState::none_required : AuthState::error;
}

std::string ExternalAudioSource::auth_instructions() const {
 std::scoped_lock lk{meta_mu_};
 return last_error_;
}

void ExternalAudioSource::start_worker() {
 std::scoped_lock lk{worker_mu_};
 if (worker_.joinable()) return;

 {
  std::scoped_lock meta_lk{meta_mu_};
  last_error_.clear();
 }

 stop_requested_.store(false, std::memory_order_release);
 worker_ = std::thread([this] { capture_loop(); });
}

void ExternalAudioSource::stop_worker() noexcept {
 std::thread t;
 {
  std::scoped_lock lk{worker_mu_};
  stop_requested_.store(true, std::memory_order_release);
  if (worker_.joinable()) t = std::move(worker_);
 }

 if (t.joinable()) {
  try {
   t.join();
  } catch (...) {
  }
 }
}

void ExternalAudioSource::append_pcm(const int16_t* data, std::size_t samples) {
 if (!data || samples == 0) return;

 std::scoped_lock lk{queue_mu_};
 compact_queue_locked();

 const std::size_t queued = pcm_queue_.size() - queue_offset_;
 if (queued + samples > kMaxQueuedSamples) {
  const std::size_t drop = queued + samples - kMaxQueuedSamples;
  queue_offset_ = std::min(queue_offset_ + drop, pcm_queue_.size());
 }

 pcm_queue_.insert(pcm_queue_.end(), data, data + samples);
}

void ExternalAudioSource::clear_queue() {
 std::scoped_lock lk{queue_mu_};
 pcm_queue_.clear();
 queue_offset_ = 0;
}

// Drop already-consumed samples once they dominate the buffer, so the queue
// doesn't grow unboundedly from the front. Caller must hold queue_mu_.
void ExternalAudioSource::compact_queue_locked() {
 if (queue_offset_ > 0 &&
  (queue_offset_ >= pcm_queue_.size() / 2u || queue_offset_ > 32768u)) {
  pcm_queue_.erase(pcm_queue_.begin(),
   pcm_queue_.begin() + static_cast<std::ptrdiff_t>(queue_offset_));
  queue_offset_ = 0;
 }
}

void ExternalAudioSource::capture_loop() noexcept {
 bool had_error = false;
 auto set_error = [this, &had_error](std::string msg) {
  had_error = true;
  {
   std::scoped_lock lk{meta_mu_};
   last_error_ = std::move(msg);
   log::warn("[external_audio] {}", last_error_);
  }
  if (!stop_requested_.load(std::memory_order_acquire)) {
   state_.store(PlaybackState::stopped, std::memory_order_release);
  }
 };

 HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
 const bool co_initialized = SUCCEEDED(hr);
 if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
  set_error(hresult_string("CoInitializeEx", hr));
  return;
 }

 auto co_uninit = [&] {
  if (co_initialized) CoUninitialize();
 };

 ComPtr<IMMDeviceEnumerator> enumerator;
 hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
  IID_PPV_ARGS(enumerator.put()));
 if (FAILED(hr) || !enumerator) {
  set_error(hresult_string("CoCreateInstance(MMDeviceEnumerator)", hr));
  co_uninit();
  return;
 }

 ComPtr<IMMDevice> device;
 hr = get_configured_endpoint(enumerator.get(), configured_endpoint(), device.put());
 if (FAILED(hr) || !device) {
  set_error(hresult_string("Get audio endpoint", hr));
  co_uninit();
  return;
 }

 std::string device_label;
 {
  auto name = friendly_name(device.get());
  device_label = name.empty() ? "Default playback device" : std::move(name);
  std::scoped_lock lk{meta_mu_};
  device_name_ = device_label;
 }

 ComPtr<IAudioClient> client;
 hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
  reinterpret_cast<void**>(client.put()));
 if (FAILED(hr) || !client) {
  set_error(hresult_string("IMMDevice::Activate(IAudioClient)", hr));
  co_uninit();
  return;
 }

 CoTaskMemWaveFormat mix;
 hr = client->GetMixFormat(mix.put());
 if (FAILED(hr) || !mix.p) {
  set_error(hresult_string("IAudioClient::GetMixFormat", hr));
  co_uninit();
  return;
 }

 InputFormat input;
 std::string parse_error;
 if (!parse_input_format(*mix.p, input, parse_error)) {
  set_error(parse_error);
  co_uninit();
  return;
 }

 constexpr REFERENCE_TIME buffer_duration_100ns = 10'000'000;
 hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
  buffer_duration_100ns, 0, mix.p, nullptr);
 if (FAILED(hr)) {
  set_error(hresult_string("IAudioClient::Initialize(loopback)", hr));
  co_uninit();
  return;
 }

 ComPtr<IAudioCaptureClient> capture;
 hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(capture.put()));
 if (FAILED(hr) || !capture) {
  set_error(hresult_string("IAudioClient::GetService(IAudioCaptureClient)", hr));
  co_uninit();
  return;
 }

 hr = client->Start();
 if (FAILED(hr)) {
  set_error(hresult_string("IAudioClient::Start", hr));
  co_uninit();
  return;
 }

 log::info("[external_audio] capturing {} ch, {} Hz, {}-bit container / {}-bit valid {} from '{}'",
  input.channels, input.sample_rate, input.container_bits, input.valid_bits,
  input.is_float ? "float" : "pcm", device_label);

 Resampler resampler;
 resampler.fmt = input;

 while (!had_error && !stop_requested_.load(std::memory_order_acquire)) {
  UINT32 next_packet_frames = 0;
  hr = capture->GetNextPacketSize(&next_packet_frames);
  if (FAILED(hr)) {
   set_error(hresult_string("IAudioCaptureClient::GetNextPacketSize", hr));
   break;
  }

  if (next_packet_frames == 0) {
   Sleep(5);
   continue;
  }

  while (next_packet_frames > 0 && !had_error && !stop_requested_.load(std::memory_order_acquire)) {
   BYTE* data = nullptr;
   UINT32 frames = 0;
   DWORD flags = 0;

   hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
   if (FAILED(hr)) {
    set_error(hresult_string("IAudioCaptureClient::GetBuffer", hr));
    break;
   }

   resampler.push_packet(data, frames, flags);
   if (!resampler.out.empty()) {
    append_pcm(resampler.out.data(), resampler.out.size());
    resampler.out.clear();
   }

   capture->ReleaseBuffer(frames);

   hr = capture->GetNextPacketSize(&next_packet_frames);
   if (FAILED(hr)) {
    set_error(hresult_string("IAudioCaptureClient::GetNextPacketSize", hr));
    next_packet_frames = 0;
    break;
   }
  }
 }

 client->Stop();
 co_uninit();
}

} // namespace fh6::sources
