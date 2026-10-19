#include "WindowsBackend.h"
#include "internal/WindowsSupport.h"

#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <propsys.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <initguid.h>
#include <devpkey.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace peripheral::platform {
namespace {
using detail::guidString;
using Microsoft::WRL::ComPtr;

// Core Audio 端点音量与静音。复用线程已有的 COM 环境，仅对本次成功初始化
// 配对 CoUninitialize；所有 COM 接口在本次调用结束时释放。
void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        std::ostringstream stream;
        stream << operation << " failed (HRESULT 0x" << std::hex << static_cast<unsigned long>(result) << ")";
        throw std::runtime_error(stream.str());
    }
}
class ComApartment {
  public:
    ComApartment() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {
        if (result_ != RPC_E_CHANGED_MODE)
            check(result_, "CoInitializeEx");
    }
    ~ComApartment() {
        if (SUCCEEDED(result_))
            CoUninitialize();
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

  private:
    HRESULT result_;
};

struct PropVariant {
    PROPVARIANT value;
    PropVariant() { PropVariantInit(&value); }
    ~PropVariant() { PropVariantClear(&value); }
    PropVariant(const PropVariant&) = delete;
    PropVariant& operator=(const PropVariant&) = delete;
};

ComPtr<IMMDeviceEnumerator> audioEnumerator() {
    ComPtr<IMMDeviceEnumerator> result;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                           IID_PPV_ARGS(result.GetAddressOf())),
          "Create audio enumerator");
    return result;
}
ComPtr<IAudioEndpointVolume> endpointVolume(IMMDevice* device) {
    ComPtr<IAudioEndpointVolume> volume;
    check(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(volume.GetAddressOf())),
          "Activate endpoint volume");
    return volume;
}
std::wstring audioName(IMMDevice* device) {
    ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, properties.GetAddressOf())))
        return {};
    PropVariant name;
    if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name.value)) &&
        name.value.vt == VT_LPWSTR && name.value.pwszVal)
        return name.value.pwszVal;
    return {};
}
std::wstring audioContainer(IMMDevice* device) {
    ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, properties.GetAddressOf())))
        return {};
    PropVariant identity;
    const PROPERTYKEY key{DEVPKEY_Device_ContainerId.fmtid, DEVPKEY_Device_ContainerId.pid};
    if (SUCCEEDED(properties->GetValue(key, &identity.value)) && identity.value.vt == VT_CLSID &&
        identity.value.puuid)
        return guidString(*identity.value.puuid);
    return {};
}
ComPtr<IMMDevice> activeAudioDevice(const std::wstring& id) {
    auto enumerator = audioEnumerator();
    ComPtr<IMMDevice> device;
    check(enumerator->GetDevice(id.c_str(), device.GetAddressOf()), "Find audio endpoint");
    DWORD state{};
    check(device->GetState(&state), "Read endpoint state");
    if (state != DEVICE_STATE_ACTIVE)
        throw std::runtime_error("Audio endpoint is no longer active");
    return device;
}

} // namespace

std::vector<AudioEndpointInfo> WindowsBackend::enumerateAudioEndpoints() const {
    ComApartment apartment;
    auto enumerator = audioEnumerator();
    ComPtr<IMMDeviceCollection> collection;
    check(enumerator->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, collection.GetAddressOf()),
          "Enumerate active audio endpoints");
    UINT count{};
    check(collection->GetCount(&count), "Count audio endpoints");
    std::vector<AudioEndpointInfo> result;
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(index, device.GetAddressOf())))
            continue;
        AudioEndpointInfo item;
        LPWSTR id{};
        if (SUCCEEDED(device->GetId(&id)) && id) {
            item.id = id;
            CoTaskMemFree(id);
        }
        item.name = audioName(device.Get());
        item.containerId = audioContainer(device.Get());
        ComPtr<IMMEndpoint> endpoint;
        EDataFlow flow = eRender;
        if (SUCCEEDED(device.As(&endpoint)) && SUCCEEDED(endpoint->GetDataFlow(&flow)))
            item.capture = flow == eCapture;
        try {
            auto volume = endpointVolume(device.Get());
            float scalar{};
            BOOL muted{};
            check(volume->GetMasterVolumeLevelScalar(&scalar), "Read endpoint volume");
            check(volume->GetMute(&muted), "Read endpoint mute");
            item.volumePercent = std::clamp(scalar * 100.0F, 0.0F, 100.0F);
            item.muted = muted != FALSE;
            item.volumeReadable = true;
        } catch (const std::exception& error) {
            item.error = error.what();
        }
        result.push_back(std::move(item));
    }
    return result;
}
void WindowsBackend::setAudioVolume(const std::wstring& endpointId, float percent) const {
    if (!std::isfinite(percent) || percent < 0 || percent > 100)
        throw std::invalid_argument("Audio volume must be between 0 and 100 percent");
    ComApartment apartment;
    auto device = activeAudioDevice(endpointId);
    auto volume = endpointVolume(device.Get());
    check(volume->SetMasterVolumeLevelScalar(percent / 100.0F, nullptr), "Set endpoint volume");
}
void WindowsBackend::setAudioMute(const std::wstring& endpointId, bool muted) const {
    ComApartment apartment;
    auto device = activeAudioDevice(endpointId);
    auto volume = endpointVolume(device.Get());
    check(volume->SetMute(muted ? TRUE : FALSE, nullptr), "Set endpoint mute");
}

} // namespace peripheral::platform
