#include "fh6/audio/wasapi_loopback_capture.hpp"

#include <windows.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <memory>
#include <string_view>

namespace fh6::audio {
namespace {

using Microsoft::WRL::ComPtr;

struct ComApartment {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ~ComApartment() {
        if (hr == S_OK || hr == S_FALSE) CoUninitialize();
    }

    bool usable() const noexcept { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

struct PropVariant {
    PROPVARIANT value{};

    PropVariant() { PropVariantInit(&value); }
    ~PropVariant() { PropVariantClear(&value); }

    PropVariant(const PropVariant&)            = delete;
    PropVariant& operator=(const PropVariant&) = delete;
};

struct CoTaskMemStringDeleter {
    void operator()(wchar_t* value) const noexcept {
        if (value) CoTaskMemFree(value);
    }
};

using CoTaskMemString = std::unique_ptr<wchar_t, CoTaskMemStringDeleter>;

std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};

    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(),
                        needed, nullptr, nullptr);
    return out;
}

std::wstring device_id(IMMDevice* device) {
    if (!device) return {};
    wchar_t* raw_id = nullptr;
    if (FAILED(device->GetId(&raw_id)) || !raw_id) return {};
    CoTaskMemString owned{raw_id};
    return owned.get();
}

std::string friendly_name(IMMDevice* device) {
    if (!device) return {};

    ComPtr<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) return {};

    PropVariant name;
    if (FAILED(props->GetValue(PKEY_Device_FriendlyName, &name.value))) return {};
    if (name.value.vt != VT_LPWSTR || !name.value.pwszVal) return {};

    return wide_to_utf8(name.value.pwszVal);
}

std::wstring default_render_id(IMMDeviceEnumerator* enumerator) {
    if (!enumerator) return {};
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) return {};
    return device_id(device.Get());
}

} // namespace

std::vector<WasapiRenderDevice> enumerate_render_devices() {
    ComApartment com;
    if (!com.usable()) return {};

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) {
        return {};
    }

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
        return {};
    }

    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) return {};

    const auto default_id = default_render_id(enumerator.Get());
    std::vector<WasapiRenderDevice> out;
    out.reserve(count);

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;

        const auto id   = device_id(device.Get());
        const auto name = friendly_name(device.Get());
        if (id.empty() || name.empty()) continue;

        out.push_back(WasapiRenderDevice{
            wide_to_utf8(id),
            name,
            !default_id.empty() && id == default_id,
        });
    }

    return out;
}

} // namespace fh6::audio
