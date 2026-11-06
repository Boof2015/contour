#pragma once
#include "model.h"
#include <windows.h>

namespace contour::app {
bool valid_setup_endpoint(std::wstring_view endpoint);
std::wstring setup_arguments(const std::filesystem::path& script,std::wstring_view endpoint,std::string_view action);
bool setup_action_allowed(const Json& report,std::string_view endpoint,std::string_view action);

// App-control-thread process owner. The inspector runs without elevation;
// mutations use a fixed helper and Windows' administrator approval dialog.
// No web-supplied path, command line, or profile crosses this boundary.
class DeviceSetup {
public:
    explicit DeviceSetup(std::filesystem::path directory) : directory_(std::move(directory)) {}
    ~DeviceSetup();
    DeviceSetup(const DeviceSetup&)=delete;
    DeviceSetup& operator=(const DeviceSetup&)=delete;
    Json request(HWND owner,std::wstring endpoint,std::string action);
    bool poll();
    bool busy() const noexcept { return process_!=nullptr; }
    bool mutating() const noexcept { return process_!=nullptr && !inspecting_; }
    const Json& state() const noexcept { return state_; }
private:
    void inspect();
    void close_process();
    void failed(const std::string& detail);
    std::filesystem::path directory_;
    std::wstring endpoint_;
    HANDLE process_=nullptr,output_=nullptr;
    bool inspecting_=false;
    ULONGLONG started_=0;
    std::string text_,notice_;
    Json state_={{"state","idle"},{"busy",false},{"canInstall",false},{"canRemove",false}};
    std::map<std::wstring,Json,std::less<>> reports_;
};
}
