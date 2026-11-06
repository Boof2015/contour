#include "setup.h"
#include <shellapi.h>
#include <algorithm>

namespace contour::app {
namespace {
std::wstring quote(std::wstring_view value) {
    if (value.find_first_of(L"\"\r\n")!=value.npos || value.find(L'\0')!=value.npos) { throw std::invalid_argument("Invalid setup path"); }
    // These are file paths, endpoint IDs and fixed words, never a shell command.
    return L"\""+std::wstring(value)+L"\"";
}
std::filesystem::path powershell() {
    wchar_t system[MAX_PATH]{};
    const auto n=GetSystemDirectoryW(system,MAX_PATH);
    if (!n || n>=MAX_PATH) { throw std::runtime_error("Cannot find Windows setup host"); }
    return std::filesystem::path(system)/L"WindowsPowerShell/v1.0/powershell.exe";
}
}
bool valid_setup_endpoint(std::wstring_view endpoint) {
    constexpr std::wstring_view prefix=L"{0.0.0.00000000}.";
    if (!endpoint.starts_with(prefix) || endpoint.size()!=prefix.size()+38) { return false; }
    const auto id=endpoint.substr(prefix.size());
    if (id.front()!=L'{' || id.back()!=L'}') { return false; }
    for (std::size_t i=1;i<37;++i) {
        if (i==9 || i==14 || i==19 || i==24) { if (id[i]!=L'-') { return false; } }
        else if (!((id[i]>=L'0' && id[i]<=L'9') || (id[i]>=L'a' && id[i]<=L'f') || (id[i]>=L'A' && id[i]<=L'F'))) { return false; }
    }
    return true;
}
std::wstring setup_arguments(const std::filesystem::path& script,std::wstring_view endpoint,std::string_view action) {
    if (!valid_setup_endpoint(endpoint)) { throw std::invalid_argument("Select an available Windows playback output"); }
    if (action!="Inspect" && action!="Install" && action!="Remove") { throw std::invalid_argument("Unknown setup action"); }
    return L"-NoLogo -NoProfile -NonInteractive -ExecutionPolicy RemoteSigned -File "+quote(script.wstring())+
        L" -Action "+wide(action)+L" -EndpointId "+quote(endpoint);
}
bool setup_action_allowed(const Json& report,std::string_view endpoint,std::string_view action) {
    if (report.value("busy",false) || report.value("endpoint",std::string{})!=endpoint) { return false; }
    return (action=="Install" && report.value("canInstall",false)) || (action=="Remove" && report.value("canRemove",false));
}
DeviceSetup::~DeviceSetup() {
    // Never terminate an elevated mutation. Normal close is guarded while it
    // runs; a process crash leaves its protected journals available on reopen.
    if (process_ && inspecting_) { TerminateProcess(process_,1); }
    close_process();
}
void DeviceSetup::close_process() {
    if (process_) { CloseHandle(process_); process_=nullptr; }
    if (output_) { CloseHandle(output_); output_=nullptr; }
}
void DeviceSetup::failed(const std::string& detail) {
    state_={{"endpoint",utf8(endpoint_)},{"state","blocked"},{"detail",detail},{"notice",notice_},
        {"busy",false},{"canInstall",false},{"canRemove",false}};
    reports_[endpoint_]=state_;
}
void DeviceSetup::inspect() {
    const auto exe=powershell();
    auto command=quote(exe.wstring())+L" "+setup_arguments(directory_/L"setup/contour-setup.ps1",endpoint_,"Inspect");
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE}; HANDLE writer=nullptr;
    if (!CreatePipe(&output_,&writer,&security,0)) { throw std::runtime_error("Cannot open setup report pipe"); }
    if (!SetHandleInformation(output_,HANDLE_FLAG_INHERIT,0)) { CloseHandle(writer); close_process(); throw std::runtime_error("Cannot protect setup pipe"); }
    STARTUPINFOW startup{}; startup.cb=sizeof(startup); startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdOutput=writer; startup.hStdError=writer;
    startup.hStdInput=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr);
    PROCESS_INFORMATION child{};
    const bool ok=CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,directory_.c_str(),&startup,&child)!=FALSE;
    if (startup.hStdInput!=INVALID_HANDLE_VALUE) { CloseHandle(startup.hStdInput); }
    CloseHandle(writer);
    if (!ok) { close_process(); throw std::runtime_error("Cannot start device inspection"); }
    process_=child.hProcess; CloseHandle(child.hThread); inspecting_=true; started_=GetTickCount64(); text_.clear();
    state_={{"endpoint",utf8(endpoint_)},{"state","checking"},{"detail","Checking this output..."},
        {"busy",true},{"canInstall",false},{"canRemove",false},{"notice",notice_}};
}
Json DeviceSetup::request(HWND owner,std::wstring endpoint,std::string action) {
    if (busy()) { throw std::runtime_error("Wait for device setup to finish"); }
    const auto script=directory_/L"setup/contour-setup.ps1";
    const auto arguments=setup_arguments(script,endpoint,action);
    if (!std::filesystem::is_regular_file(script)) { throw std::runtime_error("Installation files are missing. Download Contour again"); }
    const auto reviewed=reports_.find(endpoint);
    if (action!="Inspect" && (reviewed==reports_.end() || !setup_action_allowed(reviewed->second,utf8(endpoint),action))) { throw std::runtime_error("Check this device before installing or uninstalling Contour"); }
    if (action!="Inspect") { state_=reviewed->second; }
    endpoint_=std::move(endpoint); notice_.clear();
    if (action=="Inspect") { inspect(); return state_; }
    const auto exe=powershell();
    SHELLEXECUTEINFOW info{}; info.cbSize=sizeof(info); info.hwnd=owner;
    info.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NO_UI; info.lpVerb=L"runas";
    info.lpFile=exe.c_str(); info.lpParameters=arguments.c_str(); info.lpDirectory=directory_.c_str(); info.nShow=SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        if (GetLastError()==ERROR_CANCELLED) { state_["notice"]="Administrator approval was cancelled. No changes were made."; return state_; }
        throw std::runtime_error("Windows could not start elevated device setup");
    }
    if (!info.hProcess) { throw std::runtime_error("Windows did not return the setup process"); }
    process_=info.hProcess; inspecting_=false; started_=GetTickCount64();
    state_["busy"]=true; state_["state"]=action=="Install" ? "installing" : "removing";
    state_["detail"]=action=="Install" ? "Installing Contour..." : "Uninstalling Contour...";
    state_["canInstall"]=false; state_["canRemove"]=false; state_["notice"]="";
    return state_;
}
bool DeviceSetup::poll() {
    if (!process_) { return false; }
    const auto drain=[&]() {
        DWORD available=0;
        while (PeekNamedPipe(output_,nullptr,0,nullptr,&available,nullptr) && available) {
            char buffer[4096]; DWORD read=0;
            if (!ReadFile(output_,buffer,std::min<DWORD>(available,sizeof(buffer)),&read,nullptr) || !read) { break; }
            text_.append(buffer,read);
            if (text_.size()>65536) { return false; }
        }
        return true;
    };
    if (inspecting_) {
        if (!drain()) { TerminateProcess(process_,1); close_process(); failed("Device inspection returned an oversized report"); return true; }
        if (GetTickCount64()-started_>60000) { TerminateProcess(process_,1); close_process(); failed("Device inspection timed out. Try checking the output again."); return true; }
    }
    if (WaitForSingleObject(process_,0)!=WAIT_OBJECT_0) { return false; }
    // The child may have written its last bytes between PeekNamedPipe and exit.
    if (inspecting_ && !drain()) { close_process(); failed("Device inspection returned an oversized report"); return true; }
    DWORD code=1; GetExitCodeProcess(process_,&code); close_process();
    if (!inspecting_) {
        notice_=code==0 ? "" : "The operation did not finish successfully. Review the output status below; recovery may be needed.";
        try { inspect(); } catch (const std::exception& e) { failed(e.what()); }
        return true;
    }
    try {
        auto report=Json::parse(text_);
        if (report.at("endpoint")!=utf8(endpoint_) || !report.at("canInstall").is_boolean() || !report.at("canRemove").is_boolean()) { throw std::runtime_error("Invalid setup report"); }
        if (code!=0) { report["canInstall"]=false; report["canRemove"]=false; }
        report["busy"]=false; report["notice"]=notice_; state_=std::move(report);
        reports_[endpoint_]=state_;
    } catch (...) { failed("Cannot read the setup report. Check that the complete setup package is beside Contour.exe."); }
    return true;
}
}
