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
    if (std::wstring_view(id) == L"--registration-only") {
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

void run(const wchar_t* dll, const wchar_t* endpoint) {
    const Com com;
    const Library library(std::filesystem::absolute(dll));
    using FactoryFunction = HRESULT (STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    const auto symbol = GetProcAddress(library.value, "DllGetClassObject");
    if (!symbol) { throw std::runtime_error("DLL does not expose DllGetClassObject"); }
    const auto get_factory = std::bit_cast<FactoryFunction>(symbol);
    std::cout << "{\"Schema\":1,\"EmbeddedManifest\":"
        << (FindResourceW(library.value, MAKEINTRESOURCEW(2), RT_MANIFEST) ? "true" : "false")
        << ",\"Endpoint\":";
    emit_endpoint(endpoint);
    std::cout << ",\"Classes\":[";
    // Endpoint lookup precedes registry redirection; mapping is process-local.
    const RegistryCapture capture;
    bool first = true;
    for (const auto& clsid : {pre_mix_clsid, post_mix_clsid}) {
        ComPtr<IClassFactory> factory;
        check(get_factory(clsid, IID_PPV_ARGS(&factory)), "get Contour factory");
        ComPtr<IAudioProcessingObject> apo;
        check(factory->CreateInstance(nullptr, IID_PPV_ARGS(&apo)), "create Contour APO");
        APO_REG_PROPERTIES* raw = nullptr;
        check(apo->GetRegistrationProperties(&raw), "read APO properties");
        const std::unique_ptr<APO_REG_PROPERTIES, decltype(&CoTaskMemFree)> properties(raw, CoTaskMemFree);
        if (properties->clsid != clsid) { throw std::runtime_error("APO identity mismatch"); }
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
        std::cerr << "Usage: contour-deploy-info <APO DLL> <playback endpoint ID | --registration-only>\n";
        return 2;
    }
    try {
        run(argv[1], argv[2]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Deployment inspection: " << error.what() << '\n';
        return 1;
    }
}
