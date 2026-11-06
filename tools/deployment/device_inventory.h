#pragma once

#include <string>
#include <vector>

namespace contour::deployment {
struct PnpDevice {
    std::wstring id, name, class_id, driver_inf, driver_version, provider, service;
    std::vector<std::wstring> hardware_ids, compatible_ids, children;
    bool present = false;
};
struct DeviceInventory {
    std::wstring endpoint_id, topology_device_id, adapter_id;
    std::vector<std::wstring> search_ids;
    std::vector<PnpDevice> matches;
};
// Read-only PnP inventory, including non-present and compatible-ID matches.
// Caller initializes COM. Does not activate IAudioClient or load an APO DLL.
DeviceInventory inspect_device_inventory(const wchar_t* endpoint_id);
}
