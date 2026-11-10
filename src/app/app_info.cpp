#include "app_info.h"
#include "version.h"
#include "../eq/file_storage.h"
#include <shellapi.h>
#include <cstring>

namespace contour::app {
namespace {
constexpr auto version_key=L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
std::string windows_string(const wchar_t* name) {
    wchar_t value[256]{};DWORD bytes=sizeof(value);
    if(RegGetValueW(HKEY_LOCAL_MACHINE,version_key,name,RRF_RT_REG_SZ|RRF_SUBKEY_WOW6464KEY,nullptr,value,&bytes)!=ERROR_SUCCESS)return {};
    return utf8(value);
}
}
Json application_info(std::string_view webview_version) {
    const auto build=windows_string(L"CurrentBuildNumber"),release=windows_string(L"DisplayVersion");
    std::string windows="Unavailable";
    if(!build.empty()) {
        DWORD revision=0,bytes=sizeof(revision);
        const bool has_revision=RegGetValueW(HKEY_LOCAL_MACHINE,version_key,L"UBR",RRF_RT_REG_DWORD|RRF_SUBKEY_WOW6464KEY,nullptr,&revision,&bytes)==ERROR_SUCCESS;
        windows=(release.empty()?"":release+" · ")+"build "+build+(has_revision?"."+std::to_string(revision):"");
    }
    return {{"version",CONTOUR_VERSION_STRING},{"architecture","x64"},{"windows",windows},
            {"webview",webview_version.empty()?"Unavailable":std::string(webview_version)}};
}
std::wstring information_link(std::string_view name) {
    if(name=="project")return L"https://github.com/Boof2015/contour";
    if(name=="issues")return L"https://github.com/Boof2015/contour/issues/new/choose";
    throw std::invalid_argument("Unknown project link");
}
void open_information_link(HWND owner,std::string_view name) {
    const auto url=information_link(name);
    SHELLEXECUTEINFOW request{sizeof(request)};
    request.fMask=SEE_MASK_NOASYNC|SEE_MASK_FLAG_NO_UI;request.hwnd=owner;
    request.lpVerb=L"open";request.lpFile=url.c_str();request.nShow=SW_SHOWNORMAL;
    if(!ShellExecuteExW(&request))throw std::runtime_error("Could not open your browser. Try again or visit github.com/Boof2015/contour.");
}
void copy_information(HWND owner,std::string_view text) {
    if(text.empty()||text.size()>64*1024||text.find('\0')!=text.npos)throw std::invalid_argument("Invalid information to copy");
    const auto value=wide(text);const auto bytes=(value.size()+1)*sizeof(wchar_t);
    struct Memory { HGLOBAL value;~Memory(){if(value)GlobalFree(value);} } memory{GlobalAlloc(GMEM_MOVEABLE,bytes)};
    if(!memory.value)throw std::runtime_error("Could not prepare the clipboard");
    const auto target=GlobalLock(memory.value);
    if(!target)throw std::runtime_error("Could not prepare the clipboard");
    std::memcpy(target,value.c_str(),bytes);GlobalUnlock(memory.value);
    if(!OpenClipboard(owner))throw std::runtime_error("The clipboard is busy. Try copying again.");
    struct Clipboard { ~Clipboard(){CloseClipboard();} } clipboard;
    if(!EmptyClipboard()||!SetClipboardData(CF_UNICODETEXT,memory.value))throw std::runtime_error("Could not copy the information. Try again.");
    memory.value=nullptr; // Windows owns the allocation after SetClipboardData.
}
Json third_party_notices(const std::filesystem::path& assets) {
    struct Notice { const char* name;std::filesystem::path path; };
    const Notice notices[]={
        {"nlohmann/json · MIT License",assets.parent_path()/L"licenses"/L"json.txt"},
        {"Manrope · SIL Open Font License",assets/L"fonts"/L"OFL.txt"},
        {"Microsoft WebView2 · License",assets.parent_path()/L"licenses"/L"WebView2.txt"},
        {"Microsoft WebView2 · Third-party notices",assets.parent_path()/L"licenses"/L"WebView2-NOTICE.txt"}};
    Json result=Json::array();
    for(const auto& notice:notices) {
        const auto text=eq::detail::read_file(notice.path,2*1024*1024);
        if(!text)throw std::runtime_error("A bundled notice is missing. Reinstall Contour to restore it.");
        result.push_back({{"name",notice.name},{"text",*text}});
    }
    return result;
}
}
