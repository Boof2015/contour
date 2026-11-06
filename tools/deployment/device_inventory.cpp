#include "device_inventory.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <devicetopology.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace contour::deployment {
namespace {
void check(HRESULT status, const char* operation) {
    if (FAILED(status)) {
        std::ostringstream message;
        message << operation << ": HRESULT 0x" << std::hex << static_cast<unsigned long>(status);
        throw std::runtime_error(message.str());
    }
}
void check_cm(CONFIGRET status, const char* operation) {
    if (status != CR_SUCCESS) { check(HRESULT_FROM_WIN32(CM_MapCrToWin32Err(status, ERROR_GEN_FAILURE)), operation); }
}
DEVINST locate(const std::wstring& id) {
    auto copy = id;
    DEVINST device{};
    check_cm(CM_Locate_DevNodeW(&device, copy.data(), CM_LOCATE_DEVNODE_PHANTOM), "locate PnP device");
    return device;
}
struct Property {
    DEVPROPTYPE type = DEVPROP_TYPE_EMPTY;
    std::vector<BYTE> bytes;
};
Property read_property(DEVINST device, const DEVPROPKEY& key) {
    Property property;
    ULONG size = 0;
    auto result = CM_Get_DevNode_PropertyW(device, &key, &property.type, nullptr, &size, 0);
    if (result == CR_NO_SUCH_VALUE) { return {}; }
    if (result != CR_BUFFER_SMALL) { check_cm(result, "size PnP property"); }
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (size > 1024 * 1024) { throw std::runtime_error("PnP property exceeds inventory limit"); }
        property.bytes.resize(size);
        result = CM_Get_DevNode_PropertyW(device, &key, &property.type, property.bytes.data(), &size, 0);
        if (result == CR_BUFFER_SMALL) { continue; }
        check_cm(result, "read PnP property");
        property.bytes.resize(size);
        return property;
    }
    throw std::runtime_error("PnP property changed during inspection; retry");
}
std::vector<std::wstring> strings(DEVINST device, const DEVPROPKEY& key, bool list) {
    const auto property = read_property(device, key);
    if (property.type == DEVPROP_TYPE_EMPTY) { return {}; }
    if (property.type != static_cast<DEVPROPTYPE>(list ? DEVPROP_TYPE_STRING_LIST : DEVPROP_TYPE_STRING) ||
        property.bytes.empty() || property.bytes.size() % sizeof(wchar_t) != 0) {
        throw std::runtime_error("Unexpected PnP string property");
    }
    // Copy to aligned storage and bound every string by the returned size.
    std::wstring text(property.bytes.size() / sizeof(wchar_t), L'\0');
    std::memcpy(text.data(), property.bytes.data(), property.bytes.size());
    // Some property providers return an empty list as one NUL. The API's
    // returned byte count remains the hard bound even without a second NUL.
    if (text.back() != L'\0') {
        throw std::runtime_error("Unterminated PnP string property");
    }
    std::vector<std::wstring> result;
    for (std::size_t start = 0; start < text.size() && text[start] != L'\0';) {
        const auto end = text.find(L'\0', start);
        result.push_back(text.substr(start, end-start));
        start = end+1;
        if (!list) { break; }
    }
    return result;
}
std::wstring string(DEVINST device, const DEVPROPKEY& key) {
    const auto values = strings(device, key, false);
    return values.empty() ? L"" : values.front();
}
std::wstring class_id(DEVINST device) {
    const auto property = read_property(device, DEVPKEY_Device_ClassGuid);
    if (property.type != DEVPROP_TYPE_GUID || property.bytes.size() != sizeof(GUID)) {
        throw std::runtime_error("Matched PnP device has no class GUID");
    }
    GUID id{};
    std::memcpy(&id, property.bytes.data(), sizeof(id));
    wchar_t text[39]{};
    StringFromGUID2(id, text, 39);
    return text;
}
bool present(DEVINST device) {
    const auto property = read_property(device, DEVPKEY_Device_IsPresent);
    if (property.type != DEVPROP_TYPE_BOOLEAN || property.bytes.size() != sizeof(DEVPROP_BOOLEAN)) {
        throw std::runtime_error("Cannot establish whether matched PnP device is present");
    }
    return property.bytes[0] != DEVPROP_FALSE;
}
std::vector<std::wstring> device_ids() {
    for (int attempt = 0; attempt < 3; ++attempt) {
        ULONG size = 0;
        check_cm(CM_Get_Device_ID_List_SizeW(&size, nullptr, CM_GETIDLIST_FILTER_NONE), "size device inventory");
        if (size > 16 * 1024 * 1024) { throw std::runtime_error("PnP inventory exceeds limit"); }
        std::vector<wchar_t> buffer(size);
        const auto result = CM_Get_Device_ID_ListW(nullptr, buffer.data(), size, CM_GETIDLIST_FILTER_NONE);
        if (result == CR_BUFFER_SMALL) { continue; }
        check_cm(result, "read device inventory");
        std::vector<std::wstring> ids;
        for (auto start = buffer.begin(); start != buffer.end() && *start;) {
            const auto end = std::find(start, buffer.end(), L'\0');
            if (end == buffer.end()) { throw std::runtime_error("Unterminated PnP inventory"); }
            ids.emplace_back(start, end);
            start = end+1;
        }
        return ids;
    }
    throw std::runtime_error("Devices changed during inventory; retry");
}
bool overlaps(const std::vector<std::wstring>& a, const std::vector<std::wstring>& b) {
    for (const auto& left : a) {
        for (const auto& right : b) {
            if (CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL) { return true; }
        }
    }
    return false;
}
}

DeviceInventory inspect_device_inventory(const wchar_t* endpoint_id) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)), "endpoint enumerator");
    ComPtr<IMMDevice> endpoint;
    check(enumerator->GetDevice(endpoint_id, &endpoint), "find selected endpoint");
    ComPtr<IMMEndpoint> flow;
    check(endpoint.As(&flow), "endpoint flow interface");
    EDataFlow direction{};
    check(flow->GetDataFlow(&direction), "endpoint flow");
    if (direction != eRender) { throw std::runtime_error("Select a playback endpoint"); }
    // Endpoint property stores do not reliably expose a PnP instance ID.
    // Follow the documented topology connection to the adapter's IMMDevice;
    // its InstanceId maps to PnP without parsing an opaque endpoint ID.
    ComPtr<IDeviceTopology> topology;
    check(endpoint->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(topology.GetAddressOf())), "read endpoint topology");
    UINT count = 0;
    check(topology->GetConnectorCount(&count), "read connector count");
    if (count != 1) { throw std::runtime_error("Endpoint has ambiguous topology; requires separate review"); }
    ComPtr<IConnector> connector;
    check(topology->GetConnector(0, &connector), "read endpoint connector");
    wchar_t* raw_id = nullptr;
    check(connector->GetDeviceIdConnectedTo(&raw_id), "read connected topology identity");
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> connected_id(raw_id, CoTaskMemFree);
    if (!connected_id || *connected_id.get() == L'\0') { throw std::runtime_error("Endpoint has no connected adapter"); }
    ComPtr<IMMDevice> adapter_device;
    check(enumerator->GetDevice(connected_id.get(), &adapter_device), "find connected adapter");
    ComPtr<IPropertyStore> store;
    check(adapter_device->OpenPropertyStore(STGM_READ, &store), "read adapter properties");
    PROPVARIANT instance{};
    check(store->GetValue(PKEY_Device_InstanceId, &instance), "read adapter PnP identity");
    DeviceInventory report;
    report.endpoint_id = endpoint_id;
    report.topology_device_id = connected_id.get();
    if (instance.vt == VT_LPWSTR && instance.pwszVal) { report.adapter_id = instance.pwszVal; }
    PropVariantClear(&instance);
    if (report.adapter_id.empty()) { throw std::runtime_error("Connected adapter has no PnP instance identity"); }
    const auto adapter = locate(report.adapter_id);
    if (class_id(adapter) != L"{4D36E96C-E325-11CE-BFC1-08002BE10318}") {
        throw std::runtime_error("Endpoint parent is not a MEDIA adapter; requires separate review");
    }
    report.search_ids = strings(adapter, DEVPKEY_Device_HardwareIds, true);
    if (report.search_ids.empty()) { throw std::runtime_error("Audio adapter has no hardware IDs"); }
    // No class/present filter: compatible-only matches and old disconnected
    // devices are part of an INF's scope too. Do not silently skip read errors.
    for (const auto& id : device_ids()) {
        const auto node = locate(id);
        auto hardware = strings(node, DEVPKEY_Device_HardwareIds, true);
        auto compatible = strings(node, DEVPKEY_Device_CompatibleIds, true);
        if (!overlaps(report.search_ids, hardware) && !overlaps(report.search_ids, compatible)) { continue; }
        PnpDevice device;
        device.id = id;
        device.hardware_ids = std::move(hardware);
        device.compatible_ids = std::move(compatible);
        device.name = string(node, DEVPKEY_Device_FriendlyName);
        if (device.name.empty()) { device.name = string(node, DEVPKEY_Device_DeviceDesc); }
        device.class_id = class_id(node);
        device.present = present(node);
        device.driver_inf = string(node, DEVPKEY_Device_DriverInfPath);
        device.driver_version = string(node, DEVPKEY_Device_DriverVersion);
        device.provider = string(node, DEVPKEY_Device_DriverProvider);
        device.service = string(node, DEVPKEY_Device_Service);
        device.children = strings(node, DEVPKEY_Device_Children, true);
        report.matches.push_back(std::move(device));
    }
    return report;
}
}
