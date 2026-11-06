#include <contour/live_control.h>
#include <contour/endpoint_session.h>
#include <propvarutil.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT result, const char* operation) {
    if (FAILED(result)) { throw std::runtime_error(std::string(operation)+" failed; HRESULT "+std::to_string(result)); }
}
std::string hex(HRESULT result) {
    std::ostringstream out; out<<"\"0x"<<std::hex<<std::setw(8)<<std::setfill('0')<<static_cast<unsigned long>(result)<<'"'; return out.str();
}
struct Handle { HANDLE value=nullptr; ~Handle() { if (value) { CloseHandle(value); } } };
void identity(std::ostream& out) {
    Handle token; if (!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value)) { throw std::runtime_error("Cannot query caller token"); }
    TOKEN_ELEVATION elevation{}; DWORD bytes=0;
    if (!GetTokenInformation(token.value,TokenElevation,&elevation,sizeof(elevation),&bytes)) { throw std::runtime_error("Cannot query elevation"); }
    GetTokenInformation(token.value,TokenIntegrityLevel,nullptr,0,&bytes);
    std::vector<BYTE> storage(bytes);
    if (!GetTokenInformation(token.value,TokenIntegrityLevel,storage.data(),bytes,&bytes)) { throw std::runtime_error("Cannot query integrity level"); }
    const auto* label=reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(storage.data());
    const auto count=*GetSidSubAuthorityCount(label->Label.Sid);
    const auto integrity=*GetSidSubAuthority(label->Label.Sid,count-1);
    out<<"\"Elevated\":"<<(elevation.TokenIsElevated ? "true" : "false")
       <<",\"Restricted\":"<<(IsTokenRestricted(token.value) ? "true" : "false")<<",\"IntegrityLevel\":"<<integrity;
}
void watch(const wchar_t* endpoint_id,const std::filesystem::path& profile_path,
           unsigned seconds,const std::filesystem::path& report) {
    using namespace contour::eq;
    if (seconds<1 || seconds>300 || std::filesystem::exists(report)) { throw std::runtime_error("Use 1-300 seconds and a new report filename"); }
    const auto text=read_profile_file(profile_path); if (!text) { throw std::runtime_error("Profile does not exist"); }
    EditSession editor(parse_profile(*text));
    // An unsaved name edit exercises retention without publishing anything.
    auto draft=editor.working().configuration;
    if (draft.layers.empty()) { draft.layers.push_back({"Lifecycle draft"}); }
    else { draft.layers[0].name="Lifecycle draft"; }
    editor.replace(std::move(draft)); const auto snapshot=editor.snapshot(); const auto checkpoint=editor.saved();
    std::ostringstream out; out<<"{\"Schema\":1,"; identity(out); out<<",\"ReadOnly\":true,\"Transitions\":[";
    bool first=true; std::string previous;
    const auto started=GetTickCount64();
    {
        EndpointSession session(editor,endpoint_id);
        do {
            const auto status=session.refresh();
            std::ostringstream state;
            state<<"\"State\":\""<<endpoint_state_name(status.state)<<"\",\"DeviceState\":"<<status.device_state
                 <<",\"DeviceChanges\":"<<session.device_change_count()<<",\"SampleRate\":"<<(status.runtime ? status.runtime->sample_rate : 0)
                 <<",\"Channels\":"<<(status.runtime ? status.runtime->channels : 0);
            if (state.str()!=previous) {
                previous=state.str(); if (!first) { out<<','; } first=false;
                out<<"{\"ElapsedMs\":"<<(GetTickCount64()-started)<<','<<previous<<'}';
                std::cout<<previous<<'\n'<<std::flush;
            }
            Sleep(100);
        } while (GetTickCount64()-started<static_cast<ULONGLONG>(seconds)*1000);
    }
    if (editor.snapshot()!=snapshot || editor.saved()!=checkpoint || !editor.can_undo() || !editor.dirty() || editor.applied()) {
        throw std::runtime_error("Read-only device observation altered editor state");
    }
    out<<"],\"DraftPreserved\":true,\"CheckpointPreserved\":true,\"UndoPreserved\":true}\n";
    std::ofstream file(report,std::ios::binary); file<<out.str(); file.close();
    if (file.fail()) { throw std::runtime_error("Cannot write lifecycle report"); }
    std::cout<<"Read-only lifecycle observation finished; draft and history preserved\n";
}
void saved_profile(const wchar_t* endpoint_id, const std::filesystem::path& profile_path,
                   const std::filesystem::path& report, bool save) {
    using namespace contour::eq;
    if (std::filesystem::exists(report)) { throw std::runtime_error("Use a new report filename"); }
    const auto text=read_profile_file(profile_path);
    if (!text) { throw std::runtime_error("Profile does not exist"); }
    EditSession editor(parse_profile(*text));
    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)),"Create endpoint enumerator");
    ComPtr<IMMDevice> endpoint; check(enumerator->GetDevice(endpoint_id,&endpoint),"Find selected endpoint");
    if (save) {
        ComPtr<IPropertyStore> writer; check(open_saved_store(endpoint.Get(),STGM_READWRITE,&writer),"Open saved settings for writing");
        save_profile_settings(editor,*writer.Get());
    }
    // Reopen separately so verification is not satisfied by a writer's cache.
    ComPtr<IPropertyStore> reader; check(open_saved_store(endpoint.Get(),STGM_READ,&reader),"Reopen saved settings");
    if (read_saved_profile(*reader.Get())!=std::optional{editor.working()}) { throw std::runtime_error("Persisted profile does not match"); }
    std::ostringstream out; out<<"{\"Schema\":1,"; identity(out);
    out<<",\"Saved\":"<<(save ? "true" : "false")<<",\"Verified\":true,\"Channels\":"<<editor.working().channels<<"}\n";
    std::ofstream file(report,std::ios::binary); file<<out.str(); file.close();
    if (file.fail()) { throw std::runtime_error("Cannot write saved settings report"); }
    std::cout<<out.str();
}
void inspect(const wchar_t* endpoint_id, const std::filesystem::path& report) {
    if (std::filesystem::exists(report)) { throw std::runtime_error("Use a new report filename"); }
    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)),"Create endpoint enumerator");
    ComPtr<IMMDevice> endpoint; check(enumerator->GetDevice(endpoint_id,&endpoint),"Find selected endpoint");
    std::ostringstream out; out<<"{\"Schema\":1,"; identity(out);
    const std::wstring id=endpoint_id;
    const auto brace=id.rfind(L'{');
    if (brace==std::wstring::npos) { throw std::runtime_error("Unexpected endpoint ID"); }
    const auto path=L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\"+id.substr(brace)+L"\\FxProperties";
    HKEY key=nullptr;
    const auto access=RegOpenKeyExW(HKEY_LOCAL_MACHINE,path.c_str(),0,KEY_CREATE_SUB_KEY|KEY_WOW64_64KEY,&key);
    if (key) { RegCloseKey(key); }
    out<<",\"ContextCreationAccess\":"<<access;
    PROPVARIANT context{}; context.vt=VT_CLSID; context.puuid=const_cast<GUID*>(&contour::eq::control_context);
    ComPtr<IAudioSystemEffectsPropertyStore> effects;
    const auto activated=endpoint->Activate(__uuidof(IAudioSystemEffectsPropertyStore),CLSCTX_ALL,&context,
        reinterpret_cast<void**>(effects.GetAddressOf()));
    out<<",\"Activation\":"<<hex(activated)<<",\"Stores\":[";
    if (SUCCEEDED(activated)) {
        for (int kind=0; kind<3; ++kind) {
            if (kind) { out<<','; }
            out<<"{\"Name\":\""<<(kind==0 ? "Default" : kind==1 ? "User" : "Volatile")<<'"';
            for (const auto mode : {STGM_READ,STGM_READWRITE}) {
                ComPtr<IPropertyStore> store;
                const auto opened=kind==0 ? effects->OpenDefaultPropertyStore(mode,&store) :
                    kind==1 ? effects->OpenUserPropertyStore(mode,&store) : effects->OpenVolatilePropertyStore(mode,&store);
                out<<(mode==STGM_READ ? ",\"Read\":" : ",\"ReadWrite\":")<<hex(opened);
                if (kind==2 && mode==STGM_READ && SUCCEEDED(opened)) {
                    const auto bytes=contour::eq::read_control_value(*store.Get(),contour::eq::preview_status_key,contour::eq::max_status_bytes);
                    out<<",\"RuntimePresent\":"<<(bytes ? "true" : "false");
                    if (bytes) {
                        const auto status=contour::eq::decode_status(*bytes);
                        out<<",\"RuntimeLive\":"<<(contour::eq::status_is_live(status,GetTickCount64()) ? "true" : "false")
                           <<",\"SampleRate\":"<<status.sample_rate<<",\"Channels\":"<<status.channels;
                    }
                }
            }
            out<<'}';
        }
    }
    out<<"]}\n";
    std::ofstream file(report,std::ios::binary); file<<out.str(); file.close();
    if (file.fail()) { throw std::runtime_error("Cannot write access report"); }
    std::cout<<out.str();
}
} // namespace
int wmain(int argc,wchar_t* argv[]) {
    const bool inspect_command=argc==4 && std::wstring_view(argv[1])==L"inspect";
    const bool saved_command=argc==5 && (std::wstring_view(argv[1])==L"save" || std::wstring_view(argv[1])==L"verify-saved");
    const bool watch_command=argc==6 && std::wstring_view(argv[1])==L"watch";
    if (!inspect_command && !saved_command && !watch_command) {
        std::cout<<"Usage: contour-control-probe inspect <playback-endpoint-ID> <new-report.json>\n"
                    "       contour-control-probe save <endpoint-ID> <profile.contour> <new-report.json>\n"
                    "       contour-control-probe verify-saved <endpoint-ID> <profile.contour> <new-report.json>\n"
                    "       contour-control-probe watch <endpoint-ID> <profile.contour> <seconds> <new-report.json>\n"
                    "Inspect and verify are read-only. Save writes only Contour's saved EQ; it does not preview or play audio.\n";
        return argc==2 && std::wstring_view(argv[1])==L"--help" ? 0 : 2;
    }
    const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if (FAILED(com)) { return 1; }
    int result=0;
    try {
        if (inspect_command) { inspect(argv[2],argv[3]); }
        else if (watch_command) {
            std::size_t used=0; const auto seconds=std::stoul(argv[4],&used);
            if (used!=std::wstring_view(argv[4]).size() || seconds>300) { throw std::runtime_error("Invalid observation duration"); }
            watch(argv[2],argv[3],seconds,argv[5]);
        }
        else { saved_profile(argv[2],argv[3],argv[4],std::wstring_view(argv[1])==L"save"); }
    }
    catch (const std::exception& e) { std::cerr<<"Control probe: "<<e.what()<<'\n'; result=1; }
    CoUninitialize(); return result;
}
