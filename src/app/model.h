#pragma once
#include <contour/profile_device_session.h>
#include <nlohmann/json.hpp>

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
private:
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
};
} // namespace contour::app
