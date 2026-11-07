#pragma once
#include <contour/profile_device_session.h>
#include <nlohmann/json.hpp>
#include "audio_connection.h"

namespace contour::app {
using Json=nlohmann::json;
std::string utf8(std::wstring_view text);
std::wstring wide(std::string_view text);
eq::Configuration configuration(const Json& value);
Json configuration_json(const eq::Configuration& value,double rate);

struct Device {
    std::wstring id;
    std::string name;
    std::size_t channels=2;
    double rate=48000;
    std::uint32_t mask=0;
    bool available=false;
    DWORD state=0;
    bool contour_running=false;
};
std::vector<Device> enumerate_devices();

// Serialized app/control thread. No command accepts arbitrary filesystem paths,
// scripts, device installation, registry paths, or a shell command.
class Model {
public:
    Model(std::filesystem::path library,std::vector<Device> devices,
          IMMDeviceEnumerator* enumerator=nullptr);
    Json dispatch(const Json& request);
    Json state(bool include_document=true);
    void poll();
    bool closed() const noexcept;
    bool close_to_tray() const noexcept { return close_to_tray_; }
    const Device& selected_device() const noexcept { return devices_[device_]; }
    const Device* find_device(std::wstring_view id) const noexcept;
    void refresh_devices();
    void check_connection(const std::wstring& endpoint);
    void suspend_connections(bool value);
private:
    void set_eq_enabled(bool enabled);
    void set_unassigned_eq(const Device& device);
    void poll_master_updates();
    void bind(std::size_t index);
    void preview();
    void inspect();
    Json close(const Json& request);
    eq::EditSession& editor();
    eq::ProfileLibrary library_;
    std::vector<Device> devices_;
    std::size_t device_=0;
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
    std::unique_ptr<eq::ProfileDeviceSession> session_;
    std::optional<std::size_t> next_device_;
    std::optional<eq::ReconciliationReport> report_;
    std::uint64_t last_inspect_=0;
    std::filesystem::path settings_path_;
    bool eq_enabled_=true;
    bool close_to_tray_=true;
    Json master_issues_=Json::array();
    std::unique_ptr<AudioConnection> connection_;
    bool connections_suspended_=false;
    struct MasterUpdate {
        std::wstring id;
        std::unique_ptr<eq::ProfileDeviceSession> assigned;
        std::unique_ptr<eq::EditSession> editor;
        std::unique_ptr<eq::EndpointSession> endpoint;
        bool persisted=false;
        std::uint64_t last_attempt=0;
    };
    std::vector<MasterUpdate> master_updates_;
    std::optional<Json> deferred_close_;
    std::uint64_t deferred_close_started_=0;
};
} // namespace contour::app
