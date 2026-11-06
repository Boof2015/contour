#include "model.h"
#include <contour/peq_import.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

namespace contour::app {
using namespace eq;
using Microsoft::WRL::ComPtr;
namespace {
constexpr std::array types{"PK","LSQ","HSQ","LPQ","HPQ","NO","AP","BP"};
void check(HRESULT hr,const char* message) { if (FAILED(hr)) { throw std::runtime_error(message); } }
std::string text(const Json& j,const char* key,std::size_t limit=256) {
    auto s=j.at(key).get<std::string>();
    if (s.empty() || s.size()>limit || s.find('\0')!=s.npos) { throw std::invalid_argument("Invalid text field"); }
    return s;
}
std::optional<UnsavedChoice> choice(const Json& j) {
    if (!j.contains("choice")) { return {}; }
    const auto s=j.at("choice").get<std::string>();
    if (s=="save") { return UnsavedChoice::save; }
    if (s=="discard") { return UnsavedChoice::discard; }
    if (s=="cancel") { return UnsavedChoice::cancel; }
    throw std::invalid_argument("Unknown unsaved-changes decision");
}
const char* close_name(CloseState s) {
    constexpr std::array names{"open","needs_decision","waiting","ready","needs_attention","closed"};
    return names.at(static_cast<std::size_t>(s));
}
const char* running_name(RunningEqState s) {
    constexpr std::array names{"unavailable","unknown","loaded_match","loaded_difference","processed_match","processed_difference","transitioning","incompatible"};
    return names.at(static_cast<std::size_t>(s));
}
const char* saved_name(SavedSettingsState s) {
    constexpr std::array names{"not_checked","unavailable","missing","matches","differs","invalid","unreadable"};
    return names.at(static_cast<std::size_t>(s));
}
Json filter_json(const Filter& f) {
    return {{"type",types.at(static_cast<std::size_t>(f.type))},{"freq",f.frequency_hz},
            {"gain",f.gain_db},{"q",f.q},{"on",f.enabled}};
}
Json layer_json(const Layer& l,double rate) {
    Json fs=Json::array();
    for (const auto& f:l.filters) {
        auto item=filter_json(f);
        // Include the native compiler's curve even for a disabled filter, so
        // the editor can display its shape. Invalid-at-rate curves stay absent.
        try {
            auto visible=f; visible.enabled=true;
            ParametricEq compiled; compiled.configure({{{"Curve",{},{visible}}}},rate,1);
            auto cs=compiled.coefficients(0); Json coefficients=Json::array();
            for (const auto& c:cs) { coefficients.push_back({c.b0,c.b1,c.b2,c.a1,c.a2}); }
            item["coefficients"]=coefficients;
        } catch (const std::invalid_argument&) { item["coefficients"]=nullptr; }
        fs.push_back(std::move(item));
    }
    return {{"name",l.name},{"all",l.channels.all},{"mask",l.channels.mask},{"gain",l.gain_db},{"on",l.enabled},{"filters",fs}};
}
}
std::string utf8(std::wstring_view s) {
    if (s.empty()) { return {}; }
    const auto size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0,nullptr,nullptr);
    if (!size) { throw std::invalid_argument("Invalid Unicode text"); }
    std::string result(static_cast<std::size_t>(size),'\0');
    WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),result.data(),size,nullptr,nullptr); return result;
}
std::wstring wide(std::string_view s) {
    if (s.empty()) { return {}; }
    const auto size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0);
    if (!size) { throw std::invalid_argument("Invalid UTF-8 text"); }
    std::wstring result(static_cast<std::size_t>(size),L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),result.data(),size); return result;
}
Configuration configuration(const Json& value) {
    Configuration result; result.bypass=value.at("bypass").get<bool>();
    const auto& layers=value.at("layers");
    if (!layers.is_array() || layers.size()>max_layers) { throw std::invalid_argument("Too many layers"); }
    for (const auto& v:layers) {
        Layer l; l.name=text(v,"name",1024); l.channels.all=v.at("all").get<bool>();
        const auto& mask=v.at("mask");
        if (!mask.is_number_unsigned() && !(mask.is_number_integer() && mask.get<std::int64_t>()>=0)) { throw std::invalid_argument("Invalid channel mask"); }
        const auto bits=mask.get<std::uint64_t>(); if (bits>UINT32_MAX) { throw std::invalid_argument("Invalid channel mask"); }
        l.channels.mask=static_cast<std::uint32_t>(bits); l.gain_db=v.at("gain").get<double>(); l.enabled=v.at("on").get<bool>();
        const auto& filters=v.at("filters");
        if (!filters.is_array() || filters.size()>max_filters_per_layer) { throw std::invalid_argument("Too many filters"); }
        for (const auto& item:filters) {
            Filter f; const auto type=text(item,"type"); const auto it=std::find(types.begin(),types.end(),type);
            if (it==types.end()) { throw std::invalid_argument("Unknown filter type"); }
            f.type=static_cast<FilterType>(it-types.begin()); f.frequency_hz=item.at("freq").get<double>();
            f.gain_db=item.at("gain").get<double>(); f.q=item.at("q").get<double>(); f.enabled=item.at("on").get<bool>();
            validate_filter(f); l.filters.push_back(f);
        }
        result.layers.push_back(std::move(l));
    }
    return result;
}
Json configuration_json(const Configuration& value,double rate) {
    Json layers=Json::array(); for (const auto& l:value.layers) { layers.push_back(layer_json(l,rate)); }
    return {{"layers",layers},{"bypass",value.bypass}};
}
std::vector<Device> enumerate_devices() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)),"Cannot enumerate Windows audio devices");
    ComPtr<IMMDeviceCollection> list; check(enumerator->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,&list),"Cannot read Windows outputs");
    UINT count=0; check(list->GetCount(&count),"Cannot count Windows outputs"); std::vector<Device> result;
    ComPtr<IMMDevice> default_device; std::wstring default_id;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender,eMultimedia,&default_device))) {
        wchar_t* id=nullptr; if (SUCCEEDED(default_device->GetId(&id))) { default_id=id; CoTaskMemFree(id); }
    }
    for (UINT i=0;i<count;++i) {
        ComPtr<IMMDevice> endpoint; if (FAILED(list->Item(i,&endpoint))) { continue; }
        wchar_t* id=nullptr; if (FAILED(endpoint->GetId(&id))) { continue; }
        Device d; d.id=id; CoTaskMemFree(id); d.name="Audio output";
        ComPtr<IPropertyStore> properties;
        if (SUCCEEDED(endpoint->OpenPropertyStore(STGM_READ,&properties))) {
            PROPVARIANT name{};
            if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName,&name)) && name.vt==VT_LPWSTR && name.pwszVal) { d.name=utf8(name.pwszVal); }
            PropVariantClear(&name);
        }
        ComPtr<IAudioClient> client;
        if (FAILED(endpoint->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(client.GetAddressOf())))) { continue; }
        WAVEFORMATEX* format=nullptr; if (FAILED(client->GetMixFormat(&format))) { continue; }
        d.rate=format->nSamplesPerSec; d.channels=format->nChannels;
        if (format->wFormatTag==WAVE_FORMAT_EXTENSIBLE && format->cbSize>=22) { d.mask=reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->dwChannelMask; }
        CoTaskMemFree(format); d.available=true;
        if (d.channels==0 || d.channels>dsp::max_channels) { continue; }
        if (std::popcount(d.mask)!=static_cast<int>(d.channels)) { d.mask=0; }
        result.push_back(std::move(d));
    }
    const auto def=std::find_if(result.begin(),result.end(),[&](const Device& d){return d.id==default_id;});
    if (def!=result.end()) { std::rotate(result.begin(),def,def+1); }
    if (result.empty()) { result.push_back({L"contour-offline-stereo","Offline editor",2,48000,3,false}); }
    return result;
}
Model::Model(std::filesystem::path path,std::vector<Device> devices,IMMDeviceEnumerator* enumerator)
    : library_(std::move(path)),devices_(std::move(devices)),enumerator_(enumerator) {
    if (devices_.empty()) { throw std::invalid_argument("No editor device"); }
    bind(0);
}
void Model::bind(std::size_t index) {
    if (index>=devices_.size()) { throw std::invalid_argument("Unknown output"); }
    auto next=std::make_unique<ProfileDeviceSession>(library_,devices_[index].id,enumerator_.Get());
    session_=std::move(next); device_=index; next_device_.reset(); inspect();
}
EditSession& Model::editor() {
    if (session_->close_status().state!=CloseState::open) { throw std::runtime_error("Finish or cancel closing before editing"); }
    const auto doc=session_->current(); if (!doc) { throw std::runtime_error("Create or select a profile first"); }
    return *doc->editor();
}
void Model::preview() {
    // Editing an unattached output remains useful offline. Do not manufacture
    // a queued preview, or repeatedly request writes to an unavailable device.
    if (session_->refresh().endpoint.state==EndpointState::ready) { session_->preview(); }
    report_.reset();
}
void Model::inspect() {
    report_=session_->reconcile(); last_inspect_=GetTickCount64();
}
bool Model::closed() const noexcept { return session_->close_status().state==CloseState::closed; }
void Model::poll() {
    const auto closing=session_->close_status().state;
    if (closing==CloseState::waiting) { session_->poll_close(); }
    if (session_->close_status().state==CloseState::ready) { session_->finish_close(); }
    if (closed() && next_device_) { bind(*next_device_); }
    if (session_->close_status().state==CloseState::open) {
        session_->refresh();
        if (GetTickCount64()-last_inspect_>=2000) { inspect(); }
    }
}
Json Model::close(const Json& request) {
    const auto action=request.value("action",std::string{"request"});
    if (action=="cancel") { session_->cancel_close(); next_device_.reset(); }
    else if (action=="retry") { session_->retry_close(); }
    else if (action=="leave_pending") { session_->finish_close(CloseCompletion::leave_device_pending); }
    else if (action=="request") { session_->request_close(choice(request)); }
    else { throw std::invalid_argument("Unknown close operation"); }
    poll(); return state();
}
Json Model::dispatch(const Json& request) {
    if (!request.is_object()) { throw std::invalid_argument("Invalid editor request"); }
    const auto op=text(request,"op",40);
    if (op=="state") { poll(); return state(); }
    if (op=="close") { return close(request); }
    if (op=="device") {
        const auto id=wide(text(request,"id",4096));
        const auto it=std::find_if(devices_.begin(),devices_.end(),[&](const Device& d){return d.id==id;});
        if (it==devices_.end()) { throw std::invalid_argument("Unknown output"); }
        const auto index=static_cast<std::size_t>(it-devices_.begin());
        if (index==device_) { return state(); }
        next_device_=index; return close(request);
    }
    if (session_->close_status().state!=CloseState::open) { throw std::runtime_error("Finish or cancel closing first"); }
    if (op=="edit") { if (editor().replace(configuration(request.at("work")))) { preview(); } }
    else if (op=="begin") { editor().begin_edit(); }
    else if (op=="end") { editor().end_edit(); }
    else if (op=="undo") { if (editor().undo()) { preview(); } }
    else if (op=="redo") { if (editor().redo()) { preview(); } }
    else if (op=="save") { session_->save(); inspect(); }
    else if (op=="revert") { session_->revert(); inspect(); }
    else if (op=="apply") { session_->synchronize(); inspect(); }
    else if (op=="inspect") { inspect(); }
    else if (op=="rename") { session_->rename(text(request,"name")); }
    else if (op=="duplicate") { session_->duplicate(text(request,"name")); }
    else if (op=="save_as") { session_->save_as(text(request,"name")); inspect(); }
    else if (op=="select") {
        const auto id=text(request,"id");
        if (library_.get(id).saved.channels!=devices_[device_].channels) { throw std::invalid_argument("This profile has a different channel count"); }
        const auto selected=session_->select(id,choice(request));
        if (selected==SelectionResult::needs_decision) { auto result=state(); result["decision"]="select"; return result; }
        inspect();
    } else if (op=="new") {
        if (session_->current() && editor().dirty()) { throw std::runtime_error("Save or revert this profile before creating another"); }
        Profile p; p.channels=devices_[device_].channels; p.validation_sample_rate=devices_[device_].rate;
        p.configuration.layers.push_back({"Layer 1"});
        const auto entry=library_.create(text(request,"name"),std::move(p)); session_->select(entry.id); inspect();
    } else if (op=="import") {
        const auto source=text(request,"text",1024*1024); const auto parsed=import_peq_text(source);
        Json issues=Json::array();
        for (const auto& issue:parsed.issues) { issues.push_back({{"line",issue.line},{"error",issue.error},{"message",issue.message}}); }
        return {{"valid",parsed.layer.has_value()},{"issues",issues},
                {"layer",parsed.layer ? layer_json(*parsed.layer,editor().working().validation_sample_rate) : Json{}}};
    } else { throw std::invalid_argument("Unknown editor operation"); }
    return state(op!="begin" && op!="end");
}
Json Model::state(bool include_document) {
    Json result; result["device"]=utf8(devices_[device_].id); result["devices"]=Json::array();
    for (const auto& d:devices_) { result["devices"].push_back({{"id",utf8(d.id)},{"name",d.name},{"channels",d.channels},{"rate",d.rate},{"mask",d.mask},{"available",d.available}}); }
    result["profiles"]=Json::array();
    for (const auto& p:library_.entries()) { result["profiles"].push_back({{"id",p.id},{"name",p.name},{"channels",p.saved.channels}}); }
    const auto& close_state=session_->close_status();
    result["closing"]={{"state",close_name(close_state.state)},{"detail",close_state.detail},{"switching",next_device_.has_value()}};
    const auto& s=session_->status();
    result["status"]={{"endpoint",endpoint_state_name(s.endpoint.state)},{"detail",s.endpoint.detail},{"copy",s.copy==DeviceCopyState::current ? "current" : s.copy==DeviceCopyState::failed ? "failed" : "pending"},
        {"copyDetail",s.copy_detail},{"previewDetail",s.preview_detail},{"applied",false},{"running","unavailable"},{"saved","not_checked"}};
    if (report_ && report_->library_current) { result["status"]["running"]=running_name(report_->running_working); result["status"]["saved"]=saved_name(report_->saved); }
    if (const auto doc=session_->current()) {
        const auto& e=*doc->editor(); const auto& p=e.working();
        const auto rate=s.endpoint.runtime ? s.endpoint.runtime->sample_rate : p.validation_sample_rate;
        result["status"]["applied"]=e.applied()==e.snapshot();
        result["editor"]={{"id",doc->id()},{"name",doc->name()},{"channels",p.channels},{"rate",p.validation_sample_rate},
            {"curveRate",rate},{"dirty",e.dirty()},{"undo",e.can_undo()},{"redo",e.can_redo()},{"revision",e.snapshot()->revision()}};
        if (include_document) {
            result["editor"]["work"]=configuration_json(p.configuration,rate);
        }
    } else { result["editor"]=nullptr; }
    return result;
}
} // namespace contour::app
