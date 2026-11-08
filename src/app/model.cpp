#include "model.h"
#include "../eq/file_storage.h"
#include <contour/peq_import.h>
#include <contour/peq_export.h>
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
std::string export_filename(std::string_view name,std::wstring_view extension=L".contour") {
    auto value=wide(name);
    for (auto& c:value) { if (c<32 || std::wstring_view(L"<>:\"/\\|?*").find(c)!=std::wstring_view::npos) { c=L'_'; } }
    if (value.size()>160) { value.resize(160); if (value.back()>=0xd800 && value.back()<=0xdbff) { value.pop_back(); } }
    while (!value.empty() && (value.back()==L'.' || value.back()==L' ')) { value.pop_back(); }
    if (value.empty()) { value=L"Profile"; }
    auto stem=value.substr(0,value.find(L'.'));
    for (auto& c:stem) { if (c>=L'a' && c<=L'z') { c-=L'a'-L'A'; } }
    const bool port=stem.size()==4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) &&
        ((stem[3]>=L'0' && stem[3]<=L'9') || stem[3]==0xb9 || stem[3]==0xb2 || stem[3]==0xb3);
    if (stem==L"CON" || stem==L"PRN" || stem==L"AUX" || stem==L"NUL" || port) { value.insert(value.begin(),L'_'); }
    value+=extension;return utf8(value);
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
    Configuration result; // Master bypass belongs to app settings, not profiles.
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
    ComPtr<IMMDeviceCollection> list; check(enumerator->EnumAudioEndpoints(eRender,DEVICE_STATEMASK_ALL,&list),"Cannot read Windows outputs");
    UINT count=0; check(list->GetCount(&count),"Cannot count Windows outputs"); std::vector<Device> result;
    ComPtr<IMMDevice> default_device; std::wstring default_id;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender,eMultimedia,&default_device))) {
        wchar_t* id=nullptr; if (SUCCEEDED(default_device->GetId(&id))) { default_id=id; CoTaskMemFree(id); }
    }
    for (UINT i=0;i<count;++i) {
        ComPtr<IMMDevice> endpoint; if (FAILED(list->Item(i,&endpoint))) { continue; }
        wchar_t* id=nullptr; if (FAILED(endpoint->GetId(&id))) { continue; }
        Device d; d.id=id; CoTaskMemFree(id); d.name="Audio output";
        if (FAILED(endpoint->GetState(&d.state))) { continue; }
        d.available=d.state==DEVICE_STATE_ACTIVE; d.channels=0; d.rate=0;
        if (d.available) {
            try {
                ComPtr<IPropertyStore> control;
                if (SUCCEEDED(open_control_store(endpoint.Get(),&control))) {
                    const auto bytes=read_control_value(*control.Get(),preview_status_key,max_status_bytes);
                    d.contour_running=bytes && status_is_live(decode_status(*bytes),GetTickCount64());
                }
            } catch (...) {} // Inventory remains useful when diagnostics are unavailable.
        }
        ComPtr<IPropertyStore> properties;
        if (SUCCEEDED(endpoint->OpenPropertyStore(STGM_READ,&properties))) {
            PROPVARIANT name{};
            if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName,&name)) && name.vt==VT_LPWSTR && name.pwszVal) { d.name=utf8(name.pwszVal); }
            PropVariantClear(&name);
        }
        ComPtr<IAudioClient> client;
        if (!d.available || FAILED(endpoint->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(client.GetAddressOf())))) { result.push_back(std::move(d)); continue; }
        WAVEFORMATEX* format=nullptr; if (FAILED(client->GetMixFormat(&format))) { result.push_back(std::move(d)); continue; }
        d.rate=format->nSamplesPerSec; d.channels=format->nChannels;
        if (format->wFormatTag==WAVE_FORMAT_EXTENSIBLE && format->cbSize>=22) { d.mask=reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->dwChannelMask; }
        CoTaskMemFree(format); d.available=true;
        if (std::popcount(d.mask)!=static_cast<int>(d.channels)) { d.mask=0; }
        result.push_back(std::move(d));
    }
    const auto def=std::find_if(result.begin(),result.end(),[&](const Device& d){return d.id==default_id;});
    if (def!=result.end()) { std::rotate(result.begin(),def,def+1); }
    if (result.empty()) { result.push_back({L"contour-offline-stereo","Offline editor",2,48000,3,false}); }
    return result;
}
Model::Model(std::filesystem::path path,std::vector<Device> devices,IMMDeviceEnumerator* enumerator)
    : library_(path),devices_(std::move(devices)),enumerator_(enumerator),settings_path_(path.wstring()+L".settings") {
    if (devices_.empty()) { throw std::invalid_argument("No editor device"); }
    // Supplying an enumerator is the test boundary: fixtures never open real
    // audio devices. Production checks run on a dedicated COM thread.
    if (!enumerator) { connection_=std::make_unique<AudioConnection>(); }
    if (const auto bytes=detail::read_file(settings_path_,4096)) {
        const auto settings=Json::parse(*bytes);
        if (settings.at("version")!=1) { throw std::runtime_error("Unrecognized Contour settings version"); }
        eq_enabled_=settings.at("eqEnabled").get<bool>();
        close_to_tray_=settings.value("closeToTray",true);
        start_in_tray_=settings.value("startInTray",false);
    }
    recovery_path_=path.wstring()+L".drafts";
    GUID recovery_id{};check(CoCreateGuid(&recovery_id),"Cannot identify the recovery copy");
    wchar_t id[40]{};StringFromGUID2(recovery_id,id,40);recovery_id_=utf8(id);
    if(const auto bytes=detail::read_file(recovery_path_,32*1024*1024)) {
        const auto saved=Json::parse(*bytes);
        if(saved.at("version")!=1 || !saved.at("drafts").is_array() || saved.at("drafts").size()>128)throw std::runtime_error("Cannot read Contour's recovery copies; the file has been kept");
        for(const auto& draft:saved.at("drafts")) {
            text(draft,"id");text(draft,"name");text(draft,"device",4096);
            parse_profile(text(draft,"text",1024*1024));
        }
        recoveries_=saved.at("drafts");
    }
    last_checkpoint_=Json{{"version",1},{"drafts",recoveries_}}.dump();
    for(const auto& entry:library_.entries())observed_profiles_.emplace(entry.id,ObservedProfile{entry.revision,entry.saved});
    // Saved assignments are durable intent. Reconcile incrementally so a
    // failed copy survives restart, even if its profile is now used by only
    // one device after the editor switched to another profile.
    for(const auto& a:library_.assignments())assignment_checks_.push_back(a.endpoint_id);
    bind(0);
}
void Model::checkpoint_draft() {
    auto drafts=recoveries_;
    const auto doc=session_->current();
    if(doc) {
        auto profile=input_recovery_&&input_profile_id_==doc->id()&&input_revision_==doc->editor()->snapshot()->revision()?*input_recovery_:doc->editor()->working();
        profile.configuration.bypass=false;
        if(profile!=doc->editor()->saved())drafts.push_back({{"id",recovery_id_},{"name",doc->name()},{"device",selected_device().name},{"text",serialize_profile(profile)}});
    }
    const auto bytes=Json{{"version",1},{"drafts",drafts}}.dump();
    if(drafts.size()>128||bytes.size()>32*1024*1024)throw std::runtime_error("Recovery storage is full. Review previous unsaved edits in Profiles.");
    if(bytes!=last_checkpoint_) { detail::write_file(recovery_path_,bytes);last_checkpoint_=bytes; }
    recovery_warning_.clear();last_checkpoint_time_=GetTickCount64();
}
const Device* Model::find_device(std::wstring_view id) const noexcept {
    const auto it=std::find_if(devices_.begin(),devices_.end(),[&](const auto& d){return d.id==id;});
    return it==devices_.end() ? nullptr : &*it;
}
void Model::refresh_devices() {
    const auto selected=selected_device(); auto devices=enumerate_devices();
    auto it=std::find_if(devices.begin(),devices.end(),[&](const auto& d){return d.id==selected.id;});
    if (it==devices.end()) { auto missing=selected; missing.available=false; missing.state=DEVICE_STATE_NOTPRESENT; devices.push_back(std::move(missing)); it=devices.end()-1; }
    for(const auto& d:devices) {
        const auto* old=find_device(d.id);
        if(d.available&&(!old||!old->available||old->channels!=d.channels||old->rate!=d.rate)&&library_.assignment(d.id)&&
            std::find(assignment_checks_.begin(),assignment_checks_.end(),d.id)==assignment_checks_.end())assignment_checks_.push_back(d.id);
    }
    device_=static_cast<std::size_t>(it-devices.begin()); devices_=std::move(devices);
}
void Model::check_connection(const std::wstring& endpoint) {
    if (connection_ && !connections_suspended_) {
        const auto* device=find_device(endpoint);
        if((device&&device->available)||library_.assignment(endpoint))connection_->request(endpoint,true);
    }
}
void Model::suspend_connections(bool value) {
    connections_suspended_=value;if(connection_)connection_->suspend(value);
}
void Model::set_eq_enabled(bool enabled) {
    // Persist intent first. A device failure is reported separately and never
    // changes a profile checkpoint or its undo history.
    detail::write_file(settings_path_,Json{{"version",1},{"eqEnabled",enabled},{"closeToTray",close_to_tray_},{"startInTray",start_in_tray_}}.dump());
    eq_enabled_=enabled; master_issues_=Json::array();master_updates_.clear();
    const auto apply=[&](ProfileDeviceSession& session,const std::wstring& id) {
        const auto& status=session.set_bypass_override(!enabled);
        const auto failure=!status.preview_detail.empty() ? status.preview_detail : status.copy==DeviceCopyState::failed ? status.copy_detail : "";
        if (!failure.empty()) { master_issues_.push_back({{"device",utf8(id)},{"detail",failure}}); }
    };
    // Keep the current draft alive; other assignments use their saved profiles.
    apply(*session_,selected_device().id);
    const auto assignments=library_.assignments();
    for (const auto& assignment:assignments) {
        if (assignment.endpoint_id==selected_device().id) { continue; }
        try {
            MasterUpdate update;update.id=assignment.endpoint_id;
            update.assigned=std::make_unique<ProfileDeviceSession>(library_,assignment.endpoint_id,enumerator_.Get(),!enabled);
            update.profile_revision=library_.get(assignment.profile_id).revision;
            apply(*update.assigned,assignment.endpoint_id);
            if(update.assigned->needs_connection()){check_connection(update.id);master_updates_.push_back(std::move(update));}
        }
        catch (const std::exception& e) { master_issues_.push_back({{"device",utf8(assignment.endpoint_id)},{"detail",e.what()}}); }
    }
    // A device can still have saved EQ after its local profile assignment was
    // removed. Preserve that EQ and update only its effective master state.
    for (const auto& device:devices_) {
        if (std::none_of(assignments.begin(),assignments.end(),[&](const auto& a){return a.endpoint_id==device.id;})) {
            try { set_unassigned_eq(device); }
            catch (const std::exception& e) { master_issues_.push_back({{"device",utf8(device.id)},{"detail",e.what()}}); }
        }
    }
    inspect();
}
void Model::set_unassigned_eq(const Device& device) {
    ComPtr<IMMDeviceEnumerator> enumerator=enumerator_;
    if (!enumerator) { check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)),"Cannot find Windows audio devices"); }
    ComPtr<IMMDevice> endpoint;
    if (FAILED(enumerator->GetDevice(device.id.c_str(),&endpoint))) { return; }
    ComPtr<IPropertyStore> saved;
    // Do not provision a context or write to devices without Contour settings.
    if (FAILED(open_saved_store(endpoint.Get(),STGM_READ,&saved))) { return; }
    const auto profile=read_saved_profile(*saved.Get()); if (!profile) { return; }
    MasterUpdate update;update.id=device.id;update.editor=std::make_unique<EditSession>(*profile);
    update.endpoint=std::make_unique<EndpointSession>(*update.editor,device.id,enumerator.Get());
    update.endpoint->set_bypass_override(!eq_enabled_);
    master_updates_.push_back(std::move(update));
    auto& pending=master_updates_.back();auto& session=*pending.endpoint;
    check_connection(device.id);
    const auto state=session.refresh().state;
    if (state==EndpointState::ready || state==EndpointState::incompatible) {
        session.persist(*profile);pending.persisted=true; session.preview();
    } else if (state==EndpointState::waiting_for_audio || state==EndpointState::unavailable) {
        session.persist_inactive(*profile);pending.persisted=true;
    } else { throw std::runtime_error(session.refresh().detail); }
}
void Model::poll_master_updates() {
    for(auto it=master_updates_.begin();it!=master_updates_.end();) {
        auto& update=*it;bool done=false;std::string failure;
        EndpointState endpoint=EndpointState::unavailable;
        try {
            if(update.assigned) {
                const auto current=update.assigned->current();
                const auto assignment=library_.assignment(update.id);
                if(!assignment||!current||assignment->profile_id!=current->id()) {it=master_updates_.erase(it);continue;}
                const auto revision=library_.get(current->id()).revision;
                if(update.profile_revision&&revision!=update.profile_revision) {
                    update.assigned=std::make_unique<ProfileDeviceSession>(library_,update.id,enumerator_.Get(),!eq_enabled_);
                    update.assigned->synchronize();
                }
                update.profile_revision=revision;
                const auto& s=update.assigned->deliver_pending();endpoint=s.endpoint.state;
                done=!update.assigned->needs_connection();
                if(update.profile_change&&s.copy==DeviceCopyState::current&&endpoint==EndpointState::unavailable)done=true;
                failure=s.copy==DeviceCopyState::failed?s.copy_detail:s.preview_detail;
            } else {
                auto& session=*update.endpoint;const auto& s=session.refresh();endpoint=s.state;
                // This update owns only the saved, unassigned EQ. Never race a
                // newly assigned profile or an editor that has taken it over.
                const auto assignments=library_.assignments();
                if(std::any_of(assignments.begin(),assignments.end(),[&](const auto& a){return a.endpoint_id==update.id;})) {
                    done=true;
                } else if(GetTickCount64()-update.last_attempt>=1000) {
                    if(!update.persisted) {
                        update.last_attempt=GetTickCount64();
                        if(endpoint==EndpointState::ready)session.persist(update.editor->saved());
                        else session.persist_inactive(update.editor->saved());
                        update.persisted=true;
                    }
                    if(endpoint==EndpointState::ready) {
                        if(s.preview==PreviewFeedback::applied)done=true;
                        else if(s.preview==PreviewFeedback::ready)session.preview();
                        else if(s.preview==PreviewFeedback::rejected)failure=s.detail;
                    }
                }
            }
        } catch(const std::exception& e) {failure=e.what();}
        master_issues_.erase(std::remove_if(master_issues_.begin(),master_issues_.end(),[&](const auto& e){return e.at("device")==utf8(update.id);}),master_issues_.end());
        if(!failure.empty())master_issues_.push_back({{"device",utf8(update.id)},{"detail",failure}});
        if(done) {it=master_updates_.erase(it);continue;}
        if(connection_&&(endpoint==EndpointState::waiting_for_audio||endpoint==EndpointState::ready))connection_->request(update.id);
        ++it;
    }
}
void Model::queue_profile_update(const std::wstring& endpoint) {
    master_updates_.erase(std::remove_if(master_updates_.begin(),master_updates_.end(),[&](const auto& u){return u.id==endpoint;}),master_updates_.end());
    master_issues_.erase(std::remove_if(master_issues_.begin(),master_issues_.end(),[&](const auto& e){return e.at("device")==utf8(endpoint);}),master_issues_.end());
    try {
        MasterUpdate update;update.id=endpoint;update.profile_change=true;
        update.assigned=std::make_unique<ProfileDeviceSession>(library_,endpoint,enumerator_.Get(),!eq_enabled_);
        if(!update.assigned->current())return;
        // Remembered profile choices outlive uninstall. Saving a profile must
        // not try to reinstall Contour or hold Quit open for that device.
        if(update.assigned->refresh().endpoint.state==EndpointState::not_attached)return;
        update.profile_revision=library_.get(update.assigned->current()->id()).revision;
        const auto& s=update.assigned->synchronize();
        const auto failure=s.copy==DeviceCopyState::failed?s.copy_detail:s.preview_detail;
        if(!failure.empty())master_issues_.push_back({{"device",utf8(endpoint)},{"detail",failure}});
        if(update.assigned->needs_connection()&&!(s.copy==DeviceCopyState::current&&s.endpoint.state==EndpointState::unavailable)) {
            check_connection(endpoint);master_updates_.push_back(std::move(update));
        }
    } catch(const std::exception& e) {master_issues_.push_back({{"device",utf8(endpoint)},{"detail",e.what()}});}
}
void Model::propagate_saved_changes(std::string_view force_profile) {
    std::vector<std::string> changed;
    for(const auto& entry:library_.entries()) {
        const auto previous=observed_profiles_.find(entry.id);
        if(previous!=observed_profiles_.end()&&previous->second.revision==entry.revision&&entry.id!=force_profile)continue;
        if(entry.id==force_profile||(previous!=observed_profiles_.end()&&previous->second.profile!=entry.saved))changed.push_back(entry.id);
        observed_profiles_[entry.id]={entry.revision,entry.saved};
    }
    std::erase_if(observed_profiles_,[&](const auto& item){return std::none_of(library_.entries().begin(),library_.entries().end(),[&](const auto& entry){return entry.id==item.first;});});
    for(const auto& a:library_.assignments())if(a.endpoint_id!=selected_device().id&&std::find(changed.begin(),changed.end(),a.profile_id)!=changed.end())queue_profile_update(a.endpoint_id);
}
void Model::reconcile_assigned_device() {
    if(assignment_checks_.empty()||GetTickCount64()-last_assignment_check_<2000||session_->close_status().state!=CloseState::open||deferred_close_)return;
    last_assignment_check_=GetTickCount64();
    const auto id=assignment_checks_.front();assignment_checks_.pop_front();
    if(std::any_of(master_updates_.begin(),master_updates_.end(),[&](const auto& u){return u.id==id;}))return;
    try {
        if(id==selected_device().id) {
            if(session_->current()&&!session_->current()->editor()->dirty()) {
                const auto report=session_->reconcile();
                if(report.saved==SavedSettingsState::differs)session_->synchronize();
                else if(report.saved==SavedSettingsState::unavailable)assignment_checks_.push_back(id);
            }
        } else {
            ProfileDeviceSession check(library_,id,enumerator_.Get(),!eq_enabled_);
            const auto report=check.reconcile();
            if(report.saved==SavedSettingsState::differs)queue_profile_update(id);
            else if(report.saved==SavedSettingsState::unavailable)assignment_checks_.push_back(id);
        }
    } catch(const std::exception& e) {master_issues_.push_back({{"device",utf8(id)},{"detail",e.what()}});}
}
void Model::bind(std::size_t index) {
    if (index>=devices_.size()) { throw std::invalid_argument("Unknown output"); }
    auto next=std::make_unique<ProfileDeviceSession>(library_,devices_[index].id,enumerator_.Get(),!eq_enabled_);
    const auto pending=next->current()&&std::any_of(master_updates_.begin(),master_updates_.end(),[&](const auto& u){return u.id==devices_[index].id;});
    if(pending)master_updates_.erase(std::remove_if(master_updates_.begin(),master_updates_.end(),[&](const auto& u){return u.id==devices_[index].id;}),master_updates_.end());
    session_=std::move(next); device_=index; next_device_.reset();
    if(pending&&session_->current())session_->synchronize();
    inspect();check_connection(selected_device().id);
}
EditSession& Model::editor() {
    if (session_->close_status().state!=CloseState::open) { throw std::runtime_error("Finish or cancel closing before editing"); }
    const auto doc=session_->current(); if (!doc) { throw std::runtime_error("Create or select a profile first"); }
    return *doc->editor();
}
void Model::preview() {
    // Retain user intent while no runtime exists. Publication resumes only for
    // this exact endpoint and after checking its current profile assignment.
    session_->preview();
    report_.reset();
}
void Model::inspect() {
    report_=session_->reconcile(); last_inspect_=GetTickCount64();
}
bool Model::closed() const noexcept { return session_->close_status().state==CloseState::closed; }
void Model::poll() {
    if(GetTickCount64()-last_checkpoint_time_>=1000) {
        try { checkpoint_draft(); } catch(const std::exception& e) { recovery_warning_=e.what();last_checkpoint_time_=GetTickCount64(); }
    }
    if(connections_suspended_)return;
    propagate_saved_changes();
    reconcile_assigned_device();
    poll_master_updates();
    if(deferred_close_&&master_updates_.empty()) {
        const auto request=*deferred_close_;deferred_close_.reset();session_->request_close(choice(request));
        shared_close_started_=GetTickCount64();propagate_saved_changes();
    }
    if(connection_ && session_->needs_connection()) {
        const auto endpoint=session_->refresh().endpoint.state;
        if(endpoint==EndpointState::waiting_for_audio || endpoint==EndpointState::ready)connection_->request(selected_device().id);
    }
    const auto closing=session_->close_status().state;
    if (closing==CloseState::waiting) { session_->poll_close(); }
    if (session_->close_status().state==CloseState::ready&&master_updates_.empty()) { session_->finish_close(); }
    if (closed() && next_device_) { bind(*next_device_); if (session_->current()) { session_->synchronize(); } }
    if (session_->close_status().state==CloseState::open) {
        const auto& status=session_->deliver_pending();
        if(session_->current()&&status.copy==DeviceCopyState::current&&status.preview_detail.empty()) {
            master_issues_.erase(std::remove_if(master_issues_.begin(),master_issues_.end(),[&](const auto& e){return e.at("device")==utf8(selected_device().id);}),master_issues_.end());
        }
        if (GetTickCount64()-last_inspect_>=2000) { inspect(); }
    }
}
Json Model::close(const Json& request) {
    const auto action=request.value("action",std::string{"request"});
    if(deferred_close_) {
        if(action=="cancel"){deferred_close_.reset();next_device_.reset();}
        else if(action=="retry"){
            deferred_close_started_=GetTickCount64();
            for(const auto& update:master_updates_)check_connection(update.id);
        }
        else if(action=="leave_pending") {
            const auto pending=*deferred_close_;deferred_close_.reset();shared_close_started_=GetTickCount64();session_->request_close(choice(pending));
            // A Save resolved here can create a newer shared-device update.
            // Attempt its persisted copy before honoring the pending exit.
            propagate_saved_changes();master_updates_.clear();
        }
        poll();return state();
    }
    if(action=="request"&&!master_updates_.empty()) {
        deferred_close_=request;deferred_close_started_=GetTickCount64();poll();return state();
    }
    if (action=="cancel") { session_->cancel_close(); next_device_.reset(); }
    else if (action=="retry") { shared_close_started_=GetTickCount64();for(const auto& update:master_updates_)check_connection(update.id);session_->retry_close(); }
    else if (action=="leave_pending") { master_updates_.clear();session_->finish_close(CloseCompletion::leave_device_pending); }
    else if (action=="request") { shared_close_started_=GetTickCount64();session_->request_close(choice(request)); }
    else { throw std::invalid_argument("Unknown close operation"); }
    if(session_->needs_connection())check_connection(selected_device().id);
    poll(); return state();
}
Json Model::dispatch(const Json& request) {
    if (!request.is_object()) { throw std::invalid_argument("Invalid editor request"); }
    const auto op=text(request,"op",40);
    if (op=="state") { poll(); return state(); }
    if (op=="checkpoint") { checkpoint_draft();return state(); }
    if (op=="devices") { refresh_devices(); return state(); }
    if (op=="close") { return close(request); }
    if (op=="device") {
        const auto id=wide(text(request,"id",4096));
        const auto it=std::find_if(devices_.begin(),devices_.end(),[&](const Device& d){return d.id==id;});
        if (it==devices_.end()) { throw std::invalid_argument("Unknown output"); }
        const auto index=static_cast<std::size_t>(it-devices_.begin());
        if (index==device_) { if (session_->current()) { session_->synchronize(); inspect(); } return state(); }
        next_device_=index; return close(request);
    }
    if (deferred_close_||session_->close_status().state!=CloseState::open) { throw std::runtime_error("Finish or cancel closing first"); }
    if(op=="recovery_input") {
        if(request.value("clear",false)){input_recovery_.reset();return Json::object();}
        const auto doc=session_->current();
        if(!doc||text(request,"id")!=doc->id()||!request.at("revision").is_number_unsigned()||request.at("revision").get<std::uint64_t>()!=editor().snapshot()->revision())throw std::invalid_argument("Input belongs to an earlier draft");
        auto profile=editor().working();profile.configuration=configuration(request.at("work"));validate_profile(profile);
        input_recovery_=std::move(profile);input_profile_id_=doc->id();input_revision_=editor().snapshot()->revision();
        return Json::object();
    }
    if (op=="preferences") {
        if(!request.contains("closeToTray")&&!request.contains("startInTray"))throw std::invalid_argument("Missing preference");
        const auto close=request.value("closeToTray",close_to_tray_),start=request.value("startInTray",start_in_tray_);
        detail::write_file(settings_path_,Json{{"version",1},{"eqEnabled",eq_enabled_},{"closeToTray",close},{"startInTray",start}}.dump());
        close_to_tray_=close;start_in_tray_=start;return state();
    }
    if(op=="recovery_keep"||op=="recovery_discard") {
        const auto id=text(request,"id");
        const auto found=std::find_if(recoveries_.begin(),recoveries_.end(),[&](const auto& r){return r.at("id")==id;});
        if(found==recoveries_.end())throw std::invalid_argument("These unsaved edits are no longer available");
        std::string created;
        if(op=="recovery_keep") {
            if(found->contains("createdId"))created=found->at("createdId").get<std::string>();
            else {
                auto name=found->at("name").get<std::string>();
                if(name.size()>240) { std::size_t end=240;while((static_cast<unsigned char>(name[end])&0xc0)==0x80)--end;name.resize(end); }
                auto profile=parse_profile(found->at("text").get<std::string>());profile.configuration.bypass=false;
                created=library_.create(name+" (recovered)",std::move(profile)).id;
                (*found)["createdId"]=created;
                // A retry after a cleanup failure reuses the already-created copy.
                checkpoint_draft();
            }
        }
        const auto before=recoveries_;recoveries_.erase(found);
        try { checkpoint_draft(); } catch(...) { recoveries_=before;throw; }
        auto result=state();if(!created.empty())result["createdProfileId"]=created;return result;
    }
    if (op=="profile_preview") {
        // Saved library thumbnails are requested on demand. The selected
        // profile already has its working draft in the editor state.
        const auto& entry=library_.get(text(request,"id"));
        return {{"id",entry.id},{"revision",entry.revision},{"channels",entry.saved.channels},
            {"rate",entry.saved.validation_sample_rate},{"work",configuration_json(entry.saved.configuration,entry.saved.validation_sample_rate)}};
    }
    if(op=="profile_adapt_preview"||op=="profile_adapt") {
        const auto id=text(request,"id");const auto& entry=library_.get(id);const auto current=session_->current();
        const bool selected=current&&current->id()==id;
        const auto draft_revision=selected?Json(current->editor()->snapshot()->revision()):Json{};
        auto profile=selected?current->editor()->working():entry.saved;
        if(op=="profile_adapt"&&connection_)refresh_devices();
        const auto& device=selected_device();
        if(!device.available||!device.channels||device.channels>32||!device.rate)throw std::invalid_argument("Connect this device before adapting a profile");
        const Json target={{"id",utf8(device.id)},{"channels",device.channels},{"rate",device.rate},{"mask",device.mask}};
        if(op=="profile_adapt_preview")return {{"id",id},{"name",entry.name},{"sourceRevision",entry.revision},{"draftRevision",draft_revision},
            {"sourceChannels",profile.channels},{"target",target},{"work",configuration_json(profile.configuration,device.rate)}};
        if(request.at("sourceRevision")!=entry.revision||request.at("draftRevision")!=draft_revision||request.at("target")!=target)throw std::invalid_argument("The profile or device changed. Reopen Adapt a copy to review it again.");
        const auto& targets=request.at("targets");
        if(!targets.is_array()||targets.size()!=profile.configuration.layers.size())throw std::invalid_argument("Review the channels for every layer");
        const auto valid_bits=(std::uint64_t{1}<<device.channels)-1;
        for(std::size_t i=0;i<targets.size();++i) {
            const bool all=targets[i].at("all").get<bool>();const auto& value=targets[i].at("mask");
            if(!value.is_number_unsigned())throw std::invalid_argument("Invalid channel selection");
            const auto mask=value.get<std::uint64_t>();
            if((all&&mask!=0)||(!all&&(mask==0||(mask&~valid_bits)!=0)))throw std::invalid_argument("Choose destination channels for every layer");
            profile.configuration.layers[i].channels={all,static_cast<std::uint32_t>(mask)};
        }
        profile.channels=device.channels;profile.validation_sample_rate=device.rate;profile.configuration.bypass=false;
        validate_profile(profile);
        const auto created=library_.create(text(request,"name"),std::move(profile));
        auto result=state();result["createdProfileId"]=created.id;return result;
    }
    if (op=="layer_export") {
        const auto current=session_->current();
        if (!current || current->id()!=text(request,"id") || !request.at("revision").is_number_unsigned() ||
            request.at("revision").get<std::uint64_t>()!=editor().snapshot()->revision()) {
            throw std::invalid_argument("Layer changed; reopen its menu and try again");
        }
        const auto& layers=editor().working().configuration.layers;
        if (!request.at("layer").is_number_unsigned() || request.at("layer").get<std::uint64_t>()>=layers.size()) {
            throw std::invalid_argument("Choose a layer to export");
        }
        const auto& layer=layers[request.at("layer").get<std::size_t>()];
        return {{"filename",export_filename(layer.name,L".txt")},{"text",export_peq_text(layer)}};
    }
    if (op=="profile_export" || op=="profile_duplicate" || op=="profile_rename" || op=="profile_delete") {
        const auto id=text(request,"id"); const auto entry=library_.get(id);
        const auto current=session_->current(); const bool selected=current && current->id()==id;
        if (op=="profile_export" || op=="profile_duplicate") {
            auto profile=selected ? current->editor()->working() : entry.saved;
            profile.configuration.bypass=false; // Global app preference is never exported/copied.
            if (op=="profile_export") { return {{"name",entry.name},{"filename",export_filename(entry.name)},{"text",serialize_profile(profile)}}; }
            const auto created=library_.create(text(request,"name"),std::move(profile));
            auto result=state(); result["createdProfileId"]=created.id; return result;
        }
        if (!request.at("revision").is_number_unsigned() || request.at("revision").get<std::uint64_t>()!=entry.revision) {
            throw LibraryConflict("Profile changed; review it before trying again");
        }
        if (op=="profile_delete") {
            if (selected) { throw std::invalid_argument("Select another profile before deleting this one"); }
            library_.erase(id,entry.revision);
        } else if (selected) { session_->rename(text(request,"name")); }
        else { library_.rename(id,entry.revision,text(request,"name")); }
        return state();
    }
    if (op=="profile_import") {
        auto profile=parse_profile(text(request,"text",1024*1024)); profile.configuration.bypass=false;
        const auto created=library_.create(text(request,"name"),std::move(profile));
        auto result=state(); result["createdProfileId"]=created.id; return result;
    }
    if (op=="eq") { set_eq_enabled(request.at("enabled").get<bool>()); }
    else if (op=="edit") { if (editor().replace(configuration(request.at("work")))) { preview(); } input_recovery_.reset(); }
    else if (op=="begin") { editor().begin_edit(); }
    else if (op=="end") { editor().end_edit(); }
    else if (op=="undo") { if (editor().undo()) { preview(); } }
    else if (op=="redo") { if (editor().redo()) { preview(); } }
    else if (op=="save") { session_->save();propagate_saved_changes(session_->current()->id());inspect(); }
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
        if (selected==SelectionResult::unchanged) { session_->synchronize(); }
        inspect();
    } else if (op=="new") {
        if (session_->current() && editor().dirty()) { throw std::runtime_error("Save or revert this profile before creating another"); }
        if (!devices_[device_].channels || !devices_[device_].rate) { throw std::runtime_error("Connect this device before creating a profile"); }
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
    propagate_saved_changes();
    if(session_->needs_connection()&&op!="begin"&&op!="end")check_connection(selected_device().id);
    return state(op!="begin" && op!="end");
}
Json Model::state(bool include_document) {
    Json result; result["device"]=utf8(devices_[device_].id); result["devices"]=Json::array();
    for (const auto& d:devices_) {
        const auto connection=connection_?connection_->status(d.id):ConnectionCheck{};
        result["devices"].push_back({{"id",utf8(d.id)},{"name",d.name},{"channels",d.channels},{"rate",d.rate},{"mask",d.mask},{"available",d.available},{"state",d.state},{"running",d.contour_running},
            {"connection",{{"checking",connection.busy},{"confirmed",connection.confirmed},{"detail",connection.detail}}}});
    }
    result["eqEnabled"]=eq_enabled_; result["masterIssues"]=master_issues_;result["closeToTray"]=close_to_tray_;result["startInTray"]=start_in_tray_;
    result["recoveries"]=Json::array();
    for(const auto& r:recoveries_)result["recoveries"].push_back({{"id",r.at("id")},{"name",r.at("name")},{"device",r.at("device")}});
    result["recoveryWarning"]=recovery_warning_;
    result["profiles"]=Json::array();
    for (const auto& p:library_.entries()) {
        Json used=Json::array();
        for (const auto& a:library_.assignments()) {
            if (a.profile_id!=p.id) { continue; }
            const auto* device=find_device(a.endpoint_id);
            const bool pending=std::any_of(master_updates_.begin(),master_updates_.end(),[&](const auto& u){return u.id==a.endpoint_id;})||
                std::any_of(master_issues_.begin(),master_issues_.end(),[&](const auto& issue){return issue.at("device")==utf8(a.endpoint_id);});
            used.push_back({{"id",utf8(a.endpoint_id)},{"name",device?device->name:"Disconnected device"},{"available",device && device->available},{"pending",pending}});
        }
        const auto current=session_->current(); const bool selected=current && current->id()==p.id;
        const auto& profile=selected ? current->editor()->working() : p.saved;
        std::size_t filters=0; for (const auto& layer:profile.configuration.layers) { filters+=layer.filters.size(); }
        result["profiles"].push_back({{"id",p.id},{"name",p.name},{"channels",p.saved.channels},{"revision",p.revision},
            {"layers",profile.configuration.layers.size()},{"filters",filters},{"usedBy",used},{"canDelete",used.empty() && !selected}});
    }
    const auto& close_state=session_->close_status();
    result["closing"]={{"state",close_name(close_state.state)},{"detail",close_state.detail},{"switching",next_device_.has_value()}};
    if(deferred_close_)result["closing"]={{"state",GetTickCount64()-deferred_close_started_<3500?"waiting":"needs_attention"},
        {"detail","An EQ change on another device has not been confirmed. Retry, keep editing, or leave the update pending."},{"switching",next_device_.has_value()}};
    else if(close_state.state==CloseState::ready&&!master_updates_.empty())result["closing"]={{"state",GetTickCount64()-shared_close_started_<3500?"waiting":"needs_attention"},
        {"detail","The profile is saved. Updates on other assigned devices are still pending."},{"switching",next_device_.has_value()}};
    const auto& s=session_->status();
    result["status"]={{"endpoint",endpoint_state_name(s.endpoint.state)},{"detail",s.endpoint.detail},{"copy",s.copy==DeviceCopyState::current ? "current" : s.copy==DeviceCopyState::failed ? "failed" : "pending"},
        {"copyDetail",s.copy_detail},{"previewDetail",s.preview_detail},{"pending",session_->needs_connection()},{"applied",false},{"running","unavailable"},{"saved","not_checked"}};
    if (report_ && report_->library_current) { result["status"]["running"]=running_name(report_->running_working); result["status"]["saved"]=saved_name(report_->saved); }
    if (const auto doc=session_->current()) {
        const auto& e=*doc->editor(); const auto& p=e.working();
        const auto rate=s.endpoint.runtime ? s.endpoint.runtime->sample_rate : p.validation_sample_rate;
        result["status"]["applied"]=e.applied()==e.snapshot();
        result["editor"]={{"id",doc->id()},{"name",doc->name()},{"channels",p.channels},{"rate",p.validation_sample_rate},
            {"curveRate",rate},{"dirty",e.dirty()},{"undo",e.can_undo()},{"redo",e.can_redo()},{"revision",e.snapshot()->revision()}};
        if (include_document) {
            result["editor"]["work"]=configuration_json(p.configuration,rate);
            result["editor"]["work"].erase("bypass");
        }
    } else { result["editor"]=nullptr; }
    return result;
}
} // namespace contour::app
