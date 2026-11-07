#include "startup.h"
#include <stdexcept>
#include <vector>

namespace contour::app {
namespace {
constexpr auto run_key=L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
struct Key { HKEY value=nullptr; ~Key(){if(value)RegCloseKey(value);} };
void check(LSTATUS code) {
    if(code!=ERROR_SUCCESS)throw std::runtime_error("Could not update Windows startup settings ("+std::to_string(code)+")");
}
}
LaunchOptions parse_launch_options(std::span<const std::wstring_view> args) {
    LaunchOptions result;
    for(std::size_t i=0;i<args.size();++i) {
        if(args[i]==L"--startup"&&!result.startup)result.startup=true;
        else if(args[i]==L"--data-dir"&&!result.data&&i+1<args.size()&&!args[i+1].empty()&&!args[i+1].starts_with(L"--"))result.data=std::filesystem::absolute(args[++i]);
        else throw std::invalid_argument("Usage: contour.exe [--startup] [--data-dir directory]");
    }
    return result;
}
StartupRegistration::StartupRegistration(std::filesystem::path executable,HKEY root):root_(root) {
    const auto path=executable.wstring();
    if(!executable.is_absolute()||path.find_first_of(L"\"\r\n")!=path.npos||path.find(L'\0')!=path.npos)throw std::invalid_argument("Invalid Contour startup path");
    command_=L"\""+path+L"\" --startup";
}
std::optional<std::wstring> StartupRegistration::read() const {
    DWORD bytes=0,type=0;
    auto result=RegGetValueW(root_,run_key,L"Contour",RRF_RT_REG_SZ,&type,nullptr,&bytes);
    if(result==ERROR_FILE_NOT_FOUND||result==ERROR_PATH_NOT_FOUND)return {};
    check(result);
    if(bytes>32768*sizeof(wchar_t))throw std::runtime_error("Contour's Windows startup entry is too large");
    std::vector<wchar_t> value(bytes/sizeof(wchar_t)+1);
    check(RegGetValueW(root_,run_key,L"Contour",RRF_RT_REG_SZ,&type,value.data(),&bytes));
    return std::wstring(value.data());
}
bool StartupRegistration::enabled() const { const auto value=read();return value&&*value==command_; }
bool StartupRegistration::registered() const { return read().has_value(); }
void StartupRegistration::set_enabled(bool enabled) {
    if(enabled) {
        if(command_.size()>260)throw std::runtime_error("This folder path is too long for Windows startup. Move Contour to a shorter path first.");
        Key key;check(RegCreateKeyExW(root_,run_key,0,nullptr,0,KEY_SET_VALUE,nullptr,&key.value,nullptr));
        check(RegSetValueExW(key.value,L"Contour",0,REG_SZ,reinterpret_cast<const BYTE*>(command_.c_str()),static_cast<DWORD>((command_.size()+1)*sizeof(wchar_t))));
    } else {
        Key key;const auto opened=RegOpenKeyExW(root_,run_key,0,KEY_SET_VALUE,&key.value);
        if(opened==ERROR_FILE_NOT_FOUND||opened==ERROR_PATH_NOT_FOUND)return;
        check(opened);const auto removed=RegDeleteValueW(key.value,L"Contour");if(removed!=ERROR_FILE_NOT_FOUND)check(removed);
    }
}
} // namespace contour::app
