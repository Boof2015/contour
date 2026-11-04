#include <windows.h>
#include <audioclient.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <array>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using Microsoft::WRL::ComPtr;

void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        std::ostringstream message;
        message << operation << " failed (HRESULT 0x" << std::hex
                << std::uppercase << std::setw(8) << std::setfill('0')
                << static_cast<unsigned long>(result) << ")";
        throw std::runtime_error(message.str());
    }
}

class ComRuntime {
public:
    ComRuntime() { check(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "CoInitializeEx"); }
    ~ComRuntime() { CoUninitialize(); }
    ComRuntime(const ComRuntime&) = delete;
    ComRuntime& operator=(const ComRuntime&) = delete;
};

struct TaskMemoryDeleter {
    void operator()(void* value) const noexcept { CoTaskMemFree(value); }
};

template <typename T>
using TaskMemory = std::unique_ptr<T, TaskMemoryDeleter>;

class PropertyValue {
public:
    PROPVARIANT value{};
    PropertyValue() { PropVariantInit(&value); }
    ~PropertyValue() { PropVariantClear(&value); }
    PropertyValue(const PropertyValue&) = delete;
    PropertyValue& operator=(const PropertyValue&) = delete;
};

std::wstring device_id(IMMDevice* device) {
    wchar_t* raw = nullptr;
    check(device->GetId(&raw), "IMMDevice::GetId");
    const TaskMemory<wchar_t> id(raw);
    return id.get();
}

std::wstring device_name(IMMDevice* device) {
    ComPtr<IPropertyStore> properties;
    check(device->OpenPropertyStore(STGM_READ, &properties), "IMMDevice::OpenPropertyStore");
    PropertyValue name;
    check(properties->GetValue(PKEY_Device_FriendlyName, &name.value),
          "IPropertyStore::GetValue(FriendlyName)");
    if (name.value.vt == VT_LPWSTR && name.value.pwszVal != nullptr) {
        return name.value.pwszVal;
    }
    return L"(unnamed endpoint)";
}

struct DefaultDevice {
    ERole role;
    const wchar_t* label;
    std::wstring id;
};

std::array<DefaultDevice, 3> find_defaults(IMMDeviceEnumerator* enumerator) {
    std::array<DefaultDevice, 3> defaults{{
        {eConsole, L"console", {}},
        {eMultimedia, L"multimedia", {}},
        {eCommunications, L"communications", {}},
    }};
    for (auto& entry : defaults) {
        ComPtr<IMMDevice> device;
        const auto result = enumerator->GetDefaultAudioEndpoint(eRender, entry.role, &device);
        if (result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
            continue;
        }
        check(result, "IMMDeviceEnumerator::GetDefaultAudioEndpoint");
        entry.id = device_id(device.Get());
    }
    return defaults;
}

void print_mix_format(IMMDevice* device) {
    ComPtr<IAudioClient> client;
    check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(client.GetAddressOf())),
          "IMMDevice::Activate(IAudioClient)");

    WAVEFORMATEX* raw = nullptr;
    check(client->GetMixFormat(&raw), "IAudioClient::GetMixFormat");
    const TaskMemory<WAVEFORMATEX> format(raw);

    std::wcout << L"  Windows mix: " << format->nSamplesPerSec << L" Hz, "
               << format->nChannels << L" channels, " << format->wBitsPerSample
               << L" bits per sample container\n";

    WORD tag = format->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        if (format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            throw std::runtime_error("Truncated WAVEFORMATEXTENSIBLE returned by audio device");
        }
        const auto* extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format.get());
        constexpr GUID float_format = {STATIC_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT};
        constexpr GUID pcm_format = {STATIC_KSDATAFORMAT_SUBTYPE_PCM};
        if (IsEqualGUID(extended->SubFormat, float_format)) {
            tag = WAVE_FORMAT_IEEE_FLOAT;
        } else if (IsEqualGUID(extended->SubFormat, pcm_format)) {
            tag = WAVE_FORMAT_PCM;
        }
        std::wcout << L"  Channel mask: 0x" << std::hex << extended->dwChannelMask
                   << std::dec << L"; valid bits: " << extended->Samples.wValidBitsPerSample
                   << L'\n';
        if (tag == WAVE_FORMAT_EXTENSIBLE) {
            wchar_t subformat[39]{};
            StringFromGUID2(extended->SubFormat, subformat, 39);
            std::wcout << L"  Subformat: " << subformat << L'\n';
        }
    }

    const wchar_t* encoding = tag == WAVE_FORMAT_IEEE_FLOAT ? L"IEEE float"
                           : tag == WAVE_FORMAT_PCM ? L"integer PCM" : L"other";
    std::wcout << L"  Encoding: " << encoding << L'\n';
}

int inspect_outputs() {
    const ComRuntime com;
    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                           IID_PPV_ARGS(&enumerator)), "Create MMDeviceEnumerator");
    const auto defaults = find_defaults(enumerator.Get());

    ComPtr<IMMDeviceCollection> devices;
    check(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices),
          "IMMDeviceEnumerator::EnumAudioEndpoints");
    UINT count = 0;
    check(devices->GetCount(&count), "IMMDeviceCollection::GetCount");

    std::wcout << L"Contour audio probe -- read-only\n"
               << L"Active playback endpoints: " << count << L"\n";
    for (const auto& entry : defaults) {
        std::wcout << L"Default " << entry.label << L": "
                   << (entry.id.empty() ? L"(none)" : entry.id) << L'\n';
    }

    bool failed = false;
    for (UINT index = 0; index < count; ++index) {
        std::wcout << L"\nEndpoint " << (index + 1) << L":\n";
        try {
            ComPtr<IMMDevice> device;
            check(devices->Item(index, &device), "IMMDeviceCollection::Item");
            const auto id = device_id(device.Get());
            std::wcout << L"  ID: " << id << L"\n  Name: " << device_name(device.Get()) << L'\n';
            for (const auto& entry : defaults) {
                if (entry.id == id) {
                    std::wcout << L"  Default role: " << entry.label << L'\n';
                }
            }
            print_mix_format(device.Get());
        } catch (const std::exception& error) {
            std::cerr << "  Could not fully inspect endpoint " << (index + 1)
                      << ": " << error.what() << '\n';
            failed = true;
        }
    }
    std::wcout << L"\nMix formats describe Windows shared-mode processing.\n"
               << L"APO compatibility and Contour processing have not been tested.\n";
    return failed ? 1 : 0;
}
} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--help") {
        std::wcout << L"Usage: contour-probe [--help]\n"
                   << L"List active playback endpoints, default roles, and Windows mix formats.\n"
                   << L"Does not play audio, install effects, or change audio settings.\n";
        return 0;
    }
    if (argc != 1) {
        std::wcerr << L"Unknown arguments. Use contour-probe --help.\n";
        return 2;
    }
    try {
        return inspect_outputs();
    } catch (const std::exception& error) {
        std::cerr << "Contour probe: " << error.what() << '\n';
        return 1;
    }
}
