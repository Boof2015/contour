// Reads a selected endpoint and captures the SDK's RegisterAPO output under a
// temporary, process-local registry mapping. Never registers a system effect.
#include <windows.h>
#include <audioenginebaseapo.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <contour/apo_ids.h>
#include "device_inventory.h"

#include <bit>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using namespace contour::apo;

void check(HRESULT result, const char* label) {
    if (FAILED(result)) {
        std::ostringstream message;
        message << label << " failed: HRESULT 0x" << std::hex << static_cast<unsigned long>(result);
        throw std::runtime_error(message.str());
    }
}
void registry(LSTATUS result, const char* label) {
    if (result != ERROR_SUCCESS) {
        check(HRESULT_FROM_WIN32(result), label);
    }
}
std::wstring guid(REFGUID value) {
    wchar_t text[39]{};
    StringFromGUID2(value, text, 39);
    return text;
}
std::string json(const std::wstring& value) {
    // ASCII JSON with escaped UTF-16 code units (including surrogate pairs).
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "\"";
    for (const wchar_t c : value) {
        if (c >= 0x20 && c < 0x7f && c != L'"' && c != L'\\') {
            result += static_cast<char>(c);
        } else {
            result += "\\u";
            for (int shift = 12; shift >= 0; shift -= 4) {
                result += digits[(c >> shift) & 15];
            }
        }
    }
    return result + '"';
}
std::string hex(const std::vector<BYTE>& bytes) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (const BYTE byte : bytes) {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}
struct Key {
    HKEY value = nullptr;
    ~Key() { if (value != nullptr) { RegCloseKey(value); } }
};
struct Com {
    Com() { check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM initialization"); }
    ~Com() { CoUninitialize(); }
};
struct Library {
    HMODULE value;
    explicit Library(const std::filesystem::path& path) : value(LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (!value) { check(HRESULT_FROM_WIN32(GetLastError()), "LoadLibraryEx"); }
    }
    ~Library() { FreeLibrary(value); }
};

void emit_inventory(const wchar_t* endpoint) {
    const Com com;
    const auto report = contour::deployment::inspect_device_inventory(endpoint);
    const auto list = [](const std::vector<std::wstring>& values) {
        std::cout << '[';
        bool first = true;
        for (const auto& value : values) {
            if (!first) { std::cout << ','; }
            first = false; std::cout << json(value);
        }
        std::cout << ']';
    };
    std::cout << "{\"Schema\":1,\"Kind\":\"ContourAdapterInventory\",\"EndpointId\":" << json(report.endpoint_id)
        << ",\"TopologyDeviceId\":" << json(report.topology_device_id) << ",\"AdapterId\":" << json(report.adapter_id)
        << ",\"SearchIds\":";
    list(report.search_ids);
    std::cout << ",\"Matches\":[";
    bool first = true;
    for (const auto& device : report.matches) {
        if (!first) { std::cout << ','; }
        first = false;
        std::cout << "{\"InstanceId\":" << json(device.id) << ",\"Name\":" << json(device.name)
            << ",\"ClassId\":" << json(device.class_id) << ",\"Present\":" << (device.present ? "true" : "false")
            << ",\"DriverInf\":" << json(device.driver_inf) << ",\"DriverVersion\":" << json(device.driver_version)
            << ",\"Provider\":" << json(device.provider) << ",\"Service\":" << json(device.service)
            << ",\"HardwareIds\":";
        list(device.hardware_ids); std::cout << ",\"CompatibleIds\":"; list(device.compatible_ids);
        std::cout << ",\"Children\":"; list(device.children); std::cout << '}';
    }
    std::cout << "]}\n";
}

class RegistryCapture {
public:
    RegistryCapture() {
        GUID unique{};
        check(CoCreateGuid(&unique), "create capture ID");
        path_ = L"Software\\ContourRegistrationProbe-" + guid(unique);
        try {
            registry(RegCreateKeyExW(HKEY_CURRENT_USER, path_.c_str(), 0, nullptr,
                REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr, &root_.value, nullptr), "create capture root");
            registry(RegCreateKeyExW(root_.value, L"Classes", 0, nullptr, REG_OPTION_NON_VOLATILE,
                KEY_ALL_ACCESS, nullptr, &classes_.value, nullptr), "create capture classes");
            registry(RegCreateKeyExW(root_.value, L"Machine", 0, nullptr, REG_OPTION_NON_VOLATILE,
                KEY_ALL_ACCESS, nullptr, &machine_.value, nullptr), "create capture machine");
            registry(RegOverridePredefKey(HKEY_CLASSES_ROOT, classes_.value), "redirect HKCR");
            classes_redirected_ = true;
            registry(RegOverridePredefKey(HKEY_LOCAL_MACHINE, machine_.value), "redirect HKLM");
            machine_redirected_ = true;
        } catch (...) {
            cleanup();
            throw;
        }
    }
    ~RegistryCapture() { cleanup(); }
    HKEY classes() const { return classes_.value; }
    void verify_machine_empty() const {
        DWORD subkeys = 0, values = 0;
        registry(RegQueryInfoKeyW(machine_.value, nullptr, nullptr, nullptr, &subkeys, nullptr,
            nullptr, &values, nullptr, nullptr, nullptr, nullptr), "inspect capture scope");
        if (subkeys != 0 || values != 0) {
            throw std::runtime_error("SDK registration wrote outside the expected class scope");
        }
    }
private:
    void cleanup() noexcept {
        if (machine_redirected_) { RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr); machine_redirected_ = false; }
        if (classes_redirected_) { RegOverridePredefKey(HKEY_CLASSES_ROOT, nullptr); classes_redirected_ = false; }
        // path_ is exclusively a freshly generated ContourRegistrationProbe GUID.
        if (root_.value != nullptr) { RegDeleteTreeW(HKEY_CURRENT_USER, path_.c_str()); }
    }
    std::wstring path_;
    Key root_, classes_, machine_;
    bool classes_redirected_ = false, machine_redirected_ = false;
};

void emit_endpoint(const wchar_t* id) {
    if (std::wstring_view(id) == L"--registration-only" ||
        std::wstring_view(id) == L"--component-registration-only") {
        std::cout << "null";
        return;
    }
    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator)), "create endpoint enumerator");
    ComPtr<IMMDevice> device;
    check(enumerator->GetDevice(id, &device), "find selected endpoint");
    ComPtr<IMMEndpoint> endpoint;
    check(device.As(&endpoint), "endpoint flow interface");
    EDataFlow flow{};
    check(endpoint->GetDataFlow(&flow), "endpoint flow");
    if (flow != eRender) { throw std::runtime_error("Selected endpoint is not a playback device"); }
    DWORD state = 0;
    check(device->GetState(&state), "endpoint state");
    if (state != DEVICE_STATE_ACTIVE) { throw std::runtime_error("Selected endpoint is not active"); }
    ComPtr<IPropertyStore> properties;
    check(device->OpenPropertyStore(STGM_READ, &properties), "endpoint properties");
    PROPVARIANT name{};
    check(properties->GetValue(PKEY_Device_FriendlyName, &name), "endpoint friendly name");
    const std::wstring friendly = name.vt == VT_LPWSTR && name.pwszVal != nullptr
        ? name.pwszVal : L"(unnamed)";
    PropVariantClear(&name);
    ComPtr<IAudioClient> client;
    check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(client.GetAddressOf())), "activate audio client");
    WAVEFORMATEX* raw = nullptr;
    check(client->GetMixFormat(&raw), "read mix format");
    const std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> mix(raw, CoTaskMemFree);
    std::cout << "{\"Id\":" << json(id) << ",\"Name\":" << json(friendly)
        << ",\"State\":" << state << ",\"Channels\":" << mix->nChannels
        << ",\"SampleRate\":" << mix->nSamplesPerSec << "}";
}

void emit_values(HKEY root, const std::wstring& path) {
    Key key;
    registry(RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &key.value), "read captured APO registration");
    DWORD count = 0, max_name = 0, max_data = 0, children = 0;
    registry(RegQueryInfoKeyW(key.value, nullptr, nullptr, nullptr, &children, nullptr,
        nullptr, &count, &max_name, &max_data, nullptr, nullptr), "captured value sizes");
    if (children != 0) { throw std::runtime_error("Unexpected APO registration subkeys"); }
    std::vector<wchar_t> name(static_cast<std::size_t>(max_name) + 1);
    std::vector<BYTE> data(max_data);
    std::cout << '[';
    for (DWORD index = 0; index < count; ++index) {
        DWORD name_size = max_name + 1, data_size = max_data, type = 0;
        registry(RegEnumValueW(key.value, index, name.data(), &name_size, nullptr,
            &type, data.data(), &data_size), "captured value");
        if (index != 0) { std::cout << ','; }
        std::cout << "{\"Name\":" << json(std::wstring(name.data(), name_size))
            << ",\"Type\":" << type << ",\"Data\":\""
            << hex(std::vector<BYTE>(data.begin(), data.begin() + data_size)) << "\"}";
    }
    std::cout << ']';
}

using Registration = std::unique_ptr<APO_REG_PROPERTIES, decltype(&CoTaskMemFree)>;

Registration read_registration(const Library& library, REFCLSID clsid) {
    using FactoryFunction = HRESULT (STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    const auto symbol = GetProcAddress(library.value, "DllGetClassObject");
    if (!symbol) { throw std::runtime_error("DLL does not expose DllGetClassObject"); }
    const auto get_factory = std::bit_cast<FactoryFunction>(symbol);
    ComPtr<IClassFactory> factory;
    check(get_factory(clsid, IID_PPV_ARGS(&factory)), "get Contour factory");
    ComPtr<IAudioProcessingObject> apo;
    check(factory->CreateInstance(nullptr, IID_PPV_ARGS(&apo)), "create Contour APO");
    APO_REG_PROPERTIES* raw = nullptr;
    check(apo->GetRegistrationProperties(&raw), "read APO properties");
    Registration properties(raw, CoTaskMemFree);
    if (!properties || properties->clsid != clsid) { throw std::runtime_error("APO identity mismatch"); }
    return properties;
}

// INF string literals: metadata is currently ASCII. Reject unsupported text
// rather than silently corrupting it through the process console code page.
std::string inf_string(std::wstring_view value) {
    std::string result = "\"";
    for (const auto ch : value) {
        if (ch < 32 || ch > 126) { throw std::runtime_error("Non-ASCII INF metadata"); }
        if (ch == L'\"' || ch == L'%') { result += static_cast<char>(ch); }
        result += static_cast<char>(ch);
    }
    return result + '"';
}

void emit_component_inf(const wchar_t* dll) {
#if !defined(_M_X64)
    throw std::runtime_error("The component package currently targets Windows 11 x64 only");
#endif
    const Com com;
    const Library library(std::filesystem::absolute(dll));
    if (FindResourceW(library.value, MAKEINTRESOURCEW(2), RT_MANIFEST)) {
        throw std::runtime_error("The APO must not contain an embedded manifest");
    }
    // Read both identities before emitting anything; an older DLL cannot be
    // accidentally packaged with new CLSIDs it does not implement.
    const auto pre = read_registration(library, component_pre_mix_clsid);
    const auto post = read_registration(library, component_post_mix_clsid);
    std::ostringstream inf;
    inf << R"(; Contour device-scoped APO component. Generated from the built DLL.
; Unsigned staging artifact. An adapter-specific extension must create this
; component as a child of the audio adapter. This INF does not attach effects.
[Version]
Signature="$WINDOWS NT$"
Class=AudioProcessingObject
ClassGuid={5989fce8-9cd0-467d-8a6a-5419e31529d4}
Provider=%Provider%
DriverVer=11/06/2026,0.1.0.0
CatalogFile=contour-apo.cat
PnpLockDown=1

[Manufacturer]
%Provider%=Contour,NTamd64.10.0...22000

[Contour.NTamd64.10.0...22000]
%Description%=Contour_Install,SWC\)";
    // The identifier contains only ASCII and no INF metacharacters.
    for (const auto ch : std::wstring_view(component_id)) { inf << static_cast<char>(ch); }
    inf << R"(

[Contour_Install.NT]
CopyFiles=Contour_Files
AddReg=Contour_Registration

[Contour_Install.NT.HW]
AddReg=Contour_Name

[Contour_Name]
HKR,,FriendlyName,,%Description%

[Contour_Install.NT.Services]
AddService=,2

[Contour_Files]
contour-apo.dll

[SourceDisksNames]
1=%Disk%

[SourceDisksFiles]
contour-apo.dll=1

[DestinationDirs]
Contour_Files=13

[SignatureAttributes]
contour-apo.dll=SignatureAttributes.PETrust

[SignatureAttributes.PETrust]
PETrust=true

[Contour_Registration]
)";
    for (const auto* p : {pre.get(), post.get()}) {
        const auto id = guid(p->clsid);
        const auto cls = inf_string(L"Classes\\CLSID\\" + id);
        const auto server = inf_string(L"Classes\\CLSID\\" + id + L"\\InprocServer32");
        inf << "HKR," << cls << ",,," << inf_string(p->szFriendlyName) << '\n'
            << "HKR," << server << ",,0x00020000,\"%13%\\contour-apo.dll\"\n"
            << "HKR," << server << ",ThreadingModel,,\"Both\"\n";
        const auto key = "HKR," + inf_string(L"AudioEngine\\AudioProcessingObjects\\" + id) + ',';
        const auto number = [&](const char* name, UINT32 value) {
            // INF's decimal parser rejects UINT_MAX; hex preserves all DWORD
            // values, including the SDK's unlimited MaxInstances sentinel.
            inf << key << name << ",0x00010001,0x" << std::hex << value << std::dec << '\n';
        };
        inf << key << "FriendlyName,," << inf_string(p->szFriendlyName) << '\n'
            << key << "Copyright,," << inf_string(p->szCopyrightInfo) << '\n';
        number("MajorVersion", p->u32MajorVersion);
        number("MinorVersion", p->u32MinorVersion);
        number("Flags", p->Flags);
        number("MinInputConnections", p->u32MinInputConnections);
        number("MaxInputConnections", p->u32MaxInputConnections);
        number("MinOutputConnections", p->u32MinOutputConnections);
        number("MaxOutputConnections", p->u32MaxOutputConnections);
        number("MaxInstances", p->u32MaxInstances);
        number("NumAPOInterfaces", p->u32NumAPOInterfaces);
        for (UINT32 index = 0; index < p->u32NumAPOInterfaces; ++index) {
            inf << key << "APOInterface" << index << ",," << inf_string(guid(p->iidAPOInterfaceList[index])) << '\n';
        }
    }
    inf << R"(
[Strings]
Provider="Contour"
Description="Contour Audio Processing"
Disk="Contour APO package"
)";
    std::cout << inf.str();
}

void run(const wchar_t* dll, const wchar_t* endpoint) {
    const Com com;
    const Library library(std::filesystem::absolute(dll));
    std::cout << "{\"Schema\":1,\"EmbeddedManifest\":"
        << (FindResourceW(library.value, MAKEINTRESOURCEW(2), RT_MANIFEST) ? "true" : "false")
        << ",\"Endpoint\":";
    emit_endpoint(endpoint);
    std::cout << ",\"Classes\":[";
    // Endpoint lookup precedes registry redirection; mapping is process-local.
    const RegistryCapture capture;
    bool first = true;
    const bool component = std::wstring_view(endpoint) == L"--component-registration-only";
    for (const auto& clsid : {component ? component_pre_mix_clsid : pre_mix_clsid,
                              component ? component_post_mix_clsid : post_mix_clsid}) {
        const auto properties = read_registration(library, clsid);
        check(RegisterAPO(properties.get()), "capture RegisterAPO");
        if (!first) { std::cout << ','; }
        first = false;
        std::cout << "{\"Clsid\":" << json(guid(clsid)) << ",\"Name\":"
            << json(properties->szFriendlyName) << ",\"Values\":";
        emit_values(capture.classes(), L"AudioEngine\\AudioProcessingObjects\\" + guid(clsid));
        std::cout << '}';
    }
    capture.verify_machine_empty();
    std::cout << "]}\n";
}
} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: contour-deploy-info <APO DLL> <playback endpoint ID | --registration-only | --component-registration-only | --component-inf>\n";
        std::cerr << "       contour-deploy-info --device-inventory <playback endpoint ID>\n";
        return 2;
    }
    try {
        if (std::wstring_view(argv[1]) == L"--device-inventory") { emit_inventory(argv[2]); }
        else if (std::wstring_view(argv[2]) == L"--component-inf") { emit_component_inf(argv[1]); }
        else { run(argv[1], argv[2]); }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Deployment inspection: " << error.what() << '\n';
        return 1;
    }
}
