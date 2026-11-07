#include "model.h"
#include "control_test_support.h"
#include "test_support.h"
#include <contour/peq_import.h>
#include <fstream>
#include <iostream>
#include <map>

using namespace contour::app;
using namespace contour::eq;
using test::require;
namespace {
constexpr auto endpoint_id=L"desktop-test-output";
class MultiEnumerator final : public control_test::RuntimeClass<control_test::RuntimeClassFlags<control_test::ClassicCom>,IMMDeviceEnumerator> {
public:
    std::map<std::wstring,Microsoft::WRL::ComPtr<IMMDevice>> devices;
    std::vector<IMMNotificationClient*> observers;
    HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow,DWORD,IMMDeviceCollection**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow,ERole,IMMDevice**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR id,IMMDevice** device) override {
        if (!device) { return E_POINTER; } *device=nullptr;
        const auto it=devices.find(id?id:L""); return it==devices.end()?E_NOTFOUND:it->second.CopyTo(device);
    }
    HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient* observer) override {
        if (!observer || std::find(observers.begin(),observers.end(),observer)!=observers.end()) { return E_INVALIDARG; }
        observers.push_back(observer); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient* observer) override {
        const auto it=std::find(observers.begin(),observers.end(),observer); if(it==observers.end())return E_INVALIDARG;
        observers.erase(it); return S_OK;
    }
};
struct Fixture {
    Microsoft::WRL::ComPtr<control_test::Store> live=Microsoft::WRL::Make<control_test::Store>();
    Microsoft::WRL::ComPtr<control_test::Store> saved=Microsoft::WRL::Make<control_test::Store>();
    Microsoft::WRL::ComPtr<control_test::Endpoint> endpoint=Microsoft::WRL::Make<control_test::Endpoint>(live.Get(),saved.Get());
    Microsoft::WRL::ComPtr<control_test::Enumerator> enumerator=Microsoft::WRL::Make<control_test::Enumerator>(endpoint_id,endpoint.Get());
    SmoothEq audio;
    std::unique_ptr<PreviewProcessor> processor;
    std::unique_ptr<Model> model;
    explicit Fixture(const std::filesystem::path& file,bool attached=true) {
        Profile p; p.configuration.layers.push_back({"Initial"});
        { ProfileLibrary library(file); const auto entry=library.create("Test profile",p); ProfileWorkspace workspace(library,endpoint_id); workspace.select(entry.id); }
        audio.initialize(ResidentBank::prepare(p.configuration,48000,2,0),960);
        processor=std::make_unique<PreviewProcessor>(audio,48000,2,"",processing_fingerprint(p.configuration,48000,2));
        if (!attached) { endpoint->activation_result=E_NOINTERFACE; }
        else { write_saved_profile(*saved.Get(),p); publish_status(); }
        model=std::make_unique<Model>(file,std::vector<Device>{{endpoint_id,"Fixture output",2,48000,3,true}},enumerator.Get());
    }
    void publish_status(){write_control_value(*live.Get(),preview_status_key,encode_status(processor->poll()));}
    void idle(){auto status=processor->poll();status.heartbeat_ms=0;write_control_value(*live.Get(),preview_status_key,encode_status(status));model->poll();}
    void output_is(double gain){
        std::vector<float> samples(4000,.25F);audio.process(samples.data(),samples.data(),2000);
        test::expect_near(samples.back(),.25*std::pow(10.,gain/20.),"Resumed audio did not use the latest requested EQ");
    }
    void process_request(){
        const auto request=read_control_value(*live.Get(),preview_request_key,max_request_bytes);
        if(request)processor->submit(*request);
        std::vector<float> buffer(4000,0.1F); audio.process(buffer.data(),buffer.data(),2000); publish_status();
    }
    void consume(){process_request();model->poll();}
    Json work(){return model->state()["editor"]["work"];}
    Json edit(Json value){return model->dispatch({{"op","edit"},{"work",std::move(value)}});}
};
void readonly(const std::filesystem::path& path){
    Fixture f(path);const auto lw=f.live->writes,sw=f.saved->writes;
    const auto state=f.model->dispatch({{"op","state"}});
    require(state["editor"]["name"]=="Test profile"&&!state["editor"]["dirty"].get<bool>(),"Startup selection");
    require(!state["status"]["applied"].get<bool>()&&state["status"]["running"]=="loaded_match","Invented startup acknowledgement");
    require(lw==f.live->writes&&sw==f.saved->writes,"Opening editor wrote endpoint settings");
    require(f.model->dispatch({{"op","close"}})["closing"]["state"]=="closed","Read-only clean close");
}
void edits(const std::filesystem::path& path){
    Fixture f(path);f.model->dispatch({{"op","begin"}});
    auto work=f.work();work["layers"][0]["gain"]=-3;f.edit(work);work["layers"][0]["gain"]=-6;f.edit(work);
    f.model->dispatch({{"op","end"}});f.consume();
    require(f.model->state()["status"]["applied"].get<bool>(),"Live editor change was not acknowledged");
    auto undone=f.model->dispatch({{"op","undo"}});require(undone["editor"]["work"]["layers"][0]["gain"]==0,"Drag was not one undo step");
    f.model->dispatch({{"op","redo"}});auto state=f.model->dispatch({{"op","save"}});
    require(!state["editor"]["dirty"].get<bool>()&&state["status"]["copy"]=="current","Save not persisted");
    f.consume();require(read_saved_profile(*f.saved.Get())->configuration.layers[0].gain_db==-6,"Windows save differs");
    auto invalid=f.work();invalid["layers"][0]["mask"]=-1;
    bool rejected=false;try{f.edit(invalid);}catch(const std::exception&){rejected=true;}
    require(rejected&&f.work()["layers"][0]["gain"]==-6,"Malformed edit damaged draft");
    require(f.model->dispatch({{"op","close"}})["closing"]["state"]=="closed","Saved applied editor would not close");
}
void import_and_curves(const std::filesystem::path& path){
    Fixture f(path);
    auto imported=f.model->dispatch({{"op","import"},{"text","Preamp: -3 dB\nFilter 1: ON PK Fc 1000 Hz Gain -6 dB Q 1"}});
    require(imported["valid"].get<bool>(),"Native import failed");
    auto work=f.work();auto layer=imported["layer"];layer["name"]="Left";layer["all"]=false;layer["mask"]=1;work["layers"]=Json::array({layer});
    layer["name"]="Right";layer["mask"]=2;layer["gain"]=-9;work["layers"].push_back(layer);
    auto state=f.edit(work);f.consume();
    const auto config=configuration(state["editor"]["work"]);ParametricEq expected;expected.configure(config,48000,2);
    std::vector<float> actual(8192),reference(8192);
    for(std::size_t i=0;i<actual.size()/2;++i){actual[2*i]=actual[2*i+1]=static_cast<float>(0.1*std::sin(i*2*3.141592653589793*1000/48000));}
    reference=actual;expected.process(reference.data(),reference.data(),4096);f.audio.process(actual.data(),actual.data(),4096);
    for(std::size_t i=7000;i<actual.size();++i)test::expect_near(actual[i],reference[i],"UI edits differ from native DSP");
    const auto cs=state["editor"]["work"]["layers"][0]["filters"][0]["coefficients"][0];
    const std::complex<double> z=std::polar(1.0,-2*3.141592653589793*1000/48000);
    const auto response=(cs[0].get<double>()+cs[1].get<double>()*z+cs[2].get<double>()*z*z)/(1.0+cs[3].get<double>()*z+cs[4].get<double>()*z*z);
    require(std::abs(20*std::log10(std::abs(response))+6)<1e-9,"Graph coefficients do not match the actual peak");
    f.processor.reset();f.audio.initialize(ResidentBank::prepare(config,96000,2,0),1920);
    f.processor=std::make_unique<PreviewProcessor>(f.audio,96000,2,"",processing_fingerprint(config,96000,2));
    f.publish_status();f.model->poll();
    require(f.model->state(false)["editor"]["curveRate"]==96000,"Lightweight status omitted the new graph sample rate");
    require(f.model->state()["editor"]["work"]["layers"][0]["filters"][0]["coefficients"][0]!=cs,"Graph retained old-rate coefficients");
    imported=f.model->dispatch({{"op","import"},{"text","Include: secret.txt\nFilter 1: ON PK Fc 1000 Hz Gain -3 dB Q 1"}});
    require(!imported["valid"].get<bool>()&&f.work()["layers"].size()==2,"Unsupported script partially imported");
}
void offline_and_exit(const std::filesystem::path& path){
    Fixture f(path,false);const auto writes=f.live->writes;
    auto work=f.work();work["layers"][0]["gain"]=-4;f.edit(work);
    require(f.live->writes==writes,"Offline edit submitted preview");
    auto close=f.model->dispatch({{"op","close"}});require(close["closing"]["state"]=="needs_decision","Lost dirty exit guard");
    f.model->dispatch({{"op","close"},{"action","cancel"}});
    require(f.work()["layers"][0]["gain"]==-4,"Cancel lost draft");
    close=f.model->dispatch({{"op","close"},{"choice","save"}});
    require(!close["editor"]["dirty"].get<bool>()&&close["closing"]["state"]=="needs_attention","Partial save claimed complete");
    require(f.model->dispatch({{"op","close"},{"action","leave_pending"}})["closing"]["state"]=="closed","Explicit pending close failed");
    ProfileLibrary reopen(path);require(reopen.entries()[0].saved.configuration.layers[0].gain_db==-4,"Offline save not durable");
}
void profile_guard(const std::filesystem::path& path){
    Fixture f(path,false);const auto initial=f.model->state()["editor"]["id"];
    auto work=f.work();work["layers"][0]["gain"]=-5;f.edit(work);
    auto copy=f.model->dispatch({{"op","duplicate"},{"name","Copy <&>"}});const auto id=copy["profiles"][1]["id"];
    const auto decision=f.model->dispatch({{"op","select"},{"id",id}});
    require(decision["decision"]=="select"&&decision["editor"]["id"]==initial,"Dirty profile switched without a decision");
    auto selected=f.model->dispatch({{"op","select"},{"id",id},{"choice","discard"}});
    require(selected["editor"]["id"]==id&&selected["editor"]["name"]=="Copy <&>","Duplicate selection failed");
    bool rejected=false;try{f.model->dispatch({{"op","execute"},{"path","anything"}});}catch(const std::exception&){rejected=true;}
    require(rejected,"Unknown bridge operation accepted");
}
std::string bytes(const std::filesystem::path& path){
    std::ifstream in(path,std::ios::binary); return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
void master_eq(const std::filesystem::path& path){
    Fixture f(path);
    auto work=f.work();work["layers"][0]["gain"]=-6;f.edit(work);f.consume();
    f.model->dispatch({{"op","save"}});f.consume();
    const auto before=f.model->state();const auto library=bytes(path);
    auto off=f.model->dispatch({{"op","eq"},{"enabled",false}});f.consume();
    require(!off["eqEnabled"].get<bool>()&&!off["editor"]["dirty"].get<bool>(),"Master toggle dirtied the profile");
    require(off["editor"]["revision"]==before["editor"]["revision"]&&off["editor"]["undo"]==before["editor"]["undo"],"Master toggle entered edit history");
    require(bytes(path)==library,"Master toggle rewrote the profile library");
    require(read_saved_profile(*f.saved.Get())->configuration.bypass,"Disabled EQ was not persisted for audio restart");
    std::vector<float> samples(4000,.25F);f.audio.process(samples.data(),samples.data(),2000);
    test::expect_near(samples.back(),.25,"Master bypass did not reach actual DSP");
    work=f.work();work["layers"][0]["gain"]=-12;f.edit(work);f.consume();
    require(f.model->state()["editor"]["dirty"].get<bool>(),"Draft edit disappeared while bypassed");
    auto undo=f.model->dispatch({{"op","undo"}});f.consume();
    require(!undo["eqEnabled"].get<bool>()&&undo["editor"]["work"]["layers"][0]["gain"]==-6,"Undo changed the master setting");
    auto copy=f.model->dispatch({{"op","duplicate"},{"name","Master test copy"}});
    f.model->dispatch({{"op","select"},{"id",copy["profiles"][1]["id"]}});f.consume();
    require(!f.model->state()["eqEnabled"].get<bool>()&&read_saved_profile(*f.saved.Get())->configuration.bypass,"Selecting a profile re-enabled EQ");
    f.model->dispatch({{"op","revert"}});f.model->dispatch({{"op","save"}});f.consume();
    { ProfileLibrary check(path);for(const auto& entry:check.entries())require(!entry.saved.configuration.bypass,"Master bypass leaked into a saved profile"); }
    const auto writes=f.saved->writes;
    f.model.reset();f.model=std::make_unique<Model>(path,std::vector<Device>{{endpoint_id,"Fixture output",2,48000,3,true}},f.enumerator.Get());
    require(!f.model->state()["eqEnabled"].get<bool>()&&f.saved->writes==writes,"Restart lost master state or implicitly wrote EQ");
    const auto draft=f.model->state()["editor"];
    f.model->dispatch({{"op","eq"},{"enabled",true}});f.consume();
    require(f.model->state()["editor"]["work"]==draft["work"]&&!f.model->state()["editor"]["dirty"].get<bool>(),"Re-enabling changed the profile");
    samples.assign(4000,.25F);f.audio.process(samples.data(),samples.data(),2000);
    test::expect_near(samples.back(),.25*std::pow(10.,-6./20.),"Re-enabling did not restore saved EQ");
    // No active stream: persist the global choice for the next audio startup.
    f.idle();
    f.model->dispatch({{"op","eq"},{"enabled",false}});
    require(read_saved_profile(*f.saved.Get())->configuration.bypass,"Idle master state did not persist");
    require(!f.model->state()["editor"]["dirty"].get<bool>(),"Idle toggle dirtied the profile");
    // The same runtime can resume with its old bank still loaded. No second
    // toggle, Save or explicit retry may be needed to finish the request.
    f.publish_status();f.model->poll();f.consume();f.output_is(0);
    require(f.model->state()["status"]["applied"].get<bool>(),"Resumed bypass was not acknowledged");
}
void profile_management(const std::filesystem::path& path){
    Fixture f(path);const auto id=f.model->state()["editor"]["id"];
    auto work=f.work();work["layers"][0]["gain"]=-7;work["layers"][0]["name"]="Left correction";
    work["layers"][0]["filters"]=Json::array({{{"type","PK"},{"freq",125},{"gain",-6},{"q",2},{"on",true}}});
    work["layers"][0]["all"]=false;work["layers"][0]["mask"]=1;
    auto right=work["layers"][0];right["name"]="Right correction";right["mask"]=2;right["gain"]=-2;right["on"]=false;work["layers"].push_back(right);
    f.edit(work);f.consume();f.model->dispatch({{"op","eq"},{"enabled",false}});f.consume();
    f.model->dispatch({{"op","rename"},{"name","CON"}});
    const auto before=f.model->state();const auto disk=bytes(path);const auto live=f.live->writes,saved=f.saved->writes;
    const auto exported=f.model->dispatch({{"op","profile_export"},{"id",id}});
    const auto profile=parse_profile(exported["text"].get<std::string>());
    const auto layer_file=f.model->dispatch({{"op","layer_export"},{"id",id},{"revision",before["editor"]["revision"]},{"layer",1u}});
    const auto layer=import_peq_text(layer_file["text"].get<std::string>());
    require(layer_file["filename"]=="Right correction.txt" && layer.layer && layer.layer->gain_db==-2 &&
        layer.layer->filters==profile.configuration.layers[1].filters && layer.layer->enabled && layer.layer->channels.all,
        "Layer export lost the working filters/gain or included layer bypass/routing");
    require(profile.configuration==configuration(work) && !profile.configuration.bypass,"Export lost layers/routing or included global bypass");
    require(exported["filename"]=="_CON.contour","Reserved Windows name was not sanitized");
    require(bytes(path)==disk && f.model->state()["editor"]==before["editor"],"Export saved a draft or changed history");
    const auto output=path.parent_path()/L"roundtrip.contour";save_profile_file(output,profile);
    const auto imported=f.model->dispatch({{"op","profile_import"},{"name","Round trip"},{"text",*read_profile_file(output)}});
    const auto copy=imported["createdProfileId"];
    require(imported["editor"]==before["editor"] && imported["eqEnabled"]==false,"Import changed active EQ or draft");
    require(f.model->dispatch({{"op","profile_export"},{"id",copy}})["text"]==exported["text"],"Profile file did not round-trip exactly");
    const auto copied=ProfileLibrary(path).get(copy.get<std::string>());
    const auto thumbnail=f.model->dispatch({{"op","profile_preview"},{"id",copy}});
    require(thumbnail["id"]==copy&&thumbnail["revision"]==copied.revision&&thumbnail["channels"]==2&&thumbnail["rate"]==48000,"Preview lost the saved profile identity/format");
    require(configuration(thumbnail["work"])==copied.saved.configuration&&!thumbnail["work"]["layers"][0]["filters"][0]["coefficients"].is_null(),"Preview omitted saved layers or native coefficients");
    require(f.model->state()["editor"]==before["editor"]&&f.live->writes==live&&f.saved->writes==saved,"Preview selected or applied a profile");
    require(!ProfileLibrary(path).assignment(L"other-output"),"Import assigned a device");
    const auto reject=[&](Json request){bool failed=false;try{f.model->dispatch(request);}catch(const std::exception&){failed=true;}require(failed,"Invalid profile action was accepted");};
    reject({{"op","layer_export"},{"id",copy},{"revision",before["editor"]["revision"]},{"layer",0u}});
    reject({{"op","layer_export"},{"id",id},{"revision",0u},{"layer",0u}});
    reject({{"op","layer_export"},{"id",id},{"revision",before["editor"]["revision"]},{"layer",-1}});
    reject({{"op","layer_export"},{"id",id},{"revision",before["editor"]["revision"]},{"layer",2u}});
    reject({{"op","profile_delete"},{"id",id},{"revision",before["profiles"][0]["revision"]}});
    f.model->dispatch({{"op","profile_rename"},{"id",copy},{"revision",copied.revision},{"name","../Invalid: name?"}});
    require(f.model->dispatch({{"op","profile_export"},{"id",copy}})["filename"]==".._Invalid_ name_.contour","Export name was treated as a path");
    const auto unchanged=bytes(path);
    reject({{"op","profile_delete"},{"id",copy},{"revision",copied.revision}});
    reject({{"op","profile_import"},{"name","Invalid"},{"text",exported["text"].get<std::string>()+"Include: anything\n"}});
    require(bytes(path)==unchanged,"Rejected management operation partly changed storage");
    auto deleted=f.model->dispatch({{"op","profile_delete"},{"id",copy},{"revision",copied.revision+1}});
    require(deleted["profiles"].size()==1 && deleted["editor"]==before["editor"],"Delete changed the selected draft");
    require(f.live->writes==live && f.saved->writes==saved,"Profile management published audio or persisted device settings");
    const auto active=deleted["profiles"][0];
    require(!active["canDelete"].get<bool>() && active["usedBy"][0]["id"]==utf8(endpoint_id),"Assigned usage was omitted from the library UI");
}
void tray_preferences(const std::filesystem::path& path){
    Fixture f(path);require(f.model->close_to_tray(),"Tray should be the default close behavior");
    auto work=f.work();work["layers"][0]["gain"]=-4;f.edit(work);f.consume();
    const auto before=f.model->state()["editor"];const auto live=f.live->writes,saved=f.saved->writes;const auto disk=bytes(path);
    const auto state=f.model->dispatch({{"op","preferences"},{"closeToTray",false}});
    require(state["closeToTray"]==false&&!f.model->close_to_tray(),"Close preference did not update");
    require(state["editor"]==before&&bytes(path)==disk&&f.live->writes==live&&f.saved->writes==saved,"Close preference changed the draft or device EQ");
    f.model->dispatch({{"op","eq"},{"enabled",false}});f.consume();
    f.model.reset();f.model=std::make_unique<Model>(path,std::vector<Device>{{endpoint_id,"Fixture output",2,48000,3,true}},f.enumerator.Get());
    require(!f.model->close_to_tray()&&f.model->state()["eqEnabled"]==false,"EQ toggle overwrote tray preference or restart lost it");
    f.model->dispatch({{"op","preferences"},{"closeToTray",true}});
    require(f.model->state()["eqEnabled"]==false,"Tray preference re-enabled global EQ");
}
void idle_edits(const std::filesystem::path& path){
    Fixture f(path);f.idle();
    auto work=f.work();work["layers"][0]["gain"]=-3;f.edit(work);
    work["layers"][0]["gain"]=-9;f.edit(work);
    require(read_saved_profile(*f.saved.Get())->configuration.layers[0].gain_db==0,"Idle draft overwrote the saved checkpoint");
    f.publish_status();f.model->poll();f.consume();f.output_is(-9);
    require(f.model->state()["editor"]["dirty"].get<bool>(),"Automatic delivery saved a draft");
    f.idle();auto state=f.model->dispatch({{"op","save"}});
    require(state["status"]["copy"]=="current"&&!state["status"]["applied"].get<bool>(),"Idle Save requires audio or invents an acknowledgement");
    require(read_saved_profile(*f.saved.Get())->configuration.layers[0].gain_db==-9,"Idle Save did not reach Windows");
    // Close must give the connection check a chance to finish, then publish
    // its final target without a manual retry when the runtime returns.
    state=f.model->dispatch({{"op","close"}});
    require(state["closing"]["state"]=="waiting","Idle close failed before its connection check");
    f.publish_status();f.model->poll();f.consume();f.output_is(-9);
    require(f.model->closed(),"Idle close did not finish after the resumed audio acknowledgement");
}
void master_devices(const std::filesystem::path& path){
    Fixture first(path),second(path.wstring()+L".second"),unassigned(path.wstring()+L".unassigned");
    first.model.reset();second.model.reset();unassigned.model.reset();
    const auto enumerator=Microsoft::WRL::Make<MultiEnumerator>();
    enumerator->devices={{endpoint_id,first.endpoint},{L"second-output",second.endpoint},{L"unassigned-output",unassigned.endpoint}};
    { ProfileLibrary library(path);ProfileWorkspace second_assignment(library,L"second-output");second_assignment.select(library.entries().front().id); }
    const auto library=bytes(path);
    const std::vector<Device> devices={{endpoint_id,"First",2,48000,3,true},{L"second-output","Second",2,48000,3,true},{L"unassigned-output","Unassigned",2,48000,3,true}};
    Model model(path,devices,enumerator.Get());
    auto work=model.state()["editor"]["work"];work["layers"][0]["gain"]=-7;
    model.dispatch({{"op","edit"},{"work",work}});
    const auto draft=model.state()["editor"];
    auto state=model.dispatch({{"op","eq"},{"enabled",false}});
    require(state["masterIssues"].empty(),"Multiple-device toggle reported a failure");
    for(auto* f:{&first,&second,&unassigned}){
        require(read_saved_profile(*f->saved.Get())->configuration.bypass,"Global toggle missed an output's saved EQ");
        const auto request=read_control_value(*f->live.Get(),preview_request_key,max_request_bytes);
        require(request && parse_profile(decode_request(*request).profile).configuration.bypass,"Global toggle missed an output's live EQ");
    }
    require(state["editor"]["work"]==draft["work"]&&state["editor"]["revision"]==draft["revision"]&&state["editor"]["dirty"].get<bool>(),"Global toggle replaced the active draft");
    require(bytes(path)==library,"Global toggle changed library assignments or profiles");
    second.saved->write_result=E_ACCESSDENIED;
    state=model.dispatch({{"op","eq"},{"enabled",true}});
    require(state["eqEnabled"].get<bool>()&&state["masterIssues"].size()==1&&state["masterIssues"][0]["device"]=="second-output","Partial device failure was hidden or lost master intent");
    require(!read_saved_profile(*first.saved.Get())->configuration.bypass&&!read_saved_profile(*unassigned.saved.Get())->configuration.bypass,"One failure prevented other devices updating");
    second.saved->write_result=S_OK;
    state=model.dispatch({{"op","eq"},{"enabled",true}});
    require(state["masterIssues"].empty()&&!read_saved_profile(*second.saved.Get())->configuration.bypass,"Retry did not complete the pending master change");
    // All outputs go idle, including an installed device without a profile
    // assignment. Retain every delivery until its own runtime acknowledges.
    for(auto* f:{&first,&second,&unassigned}) {
        auto status=f->processor->poll();status.heartbeat_ms=0;
        write_control_value(*f->live.Get(),preview_status_key,encode_status(status));
    }
    model.dispatch({{"op","eq"},{"enabled",false}});
    for(auto* f:{&first,&second,&unassigned})f->publish_status();
    model.poll();
    for(auto* f:{&first,&second,&unassigned}) {
        const auto request=read_control_value(*f->live.Get(),preview_request_key,max_request_bytes);
        require(request&&parse_profile(decode_request(*request).profile).configuration.bypass,"Idle global toggle missed a resumed output");
    }
    state=model.dispatch({{"op","close"},{"choice","save"}});
    require(state["closing"]["state"]=="waiting"&&!model.closed(),"Close abandoned another output's pending EQ");
    for(int pass=0;pass<3&&!model.closed();++pass) {
        for(auto* f:{&first,&second,&unassigned})f->process_request();
        model.poll();
    }
    require(model.closed(),"Close did not complete after all output acknowledgements");
    for(auto* f:{&first,&second,&unassigned})f->output_is(0);
}
}
int wmain(int argc,wchar_t** argv){
    // Explicit manual integration check; never part of automated CTest. It
    // only opens the requested device silently and reads existing settings.
    if(argc==3&&std::wstring_view(argv[1])==L"--check-connection") {
        if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)))return 2;
        int result=1;
        try {
            Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
            require(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator))),"Enumerate devices");
            Microsoft::WRL::ComPtr<IMMDevice> device;require(SUCCEEDED(enumerator->GetDevice(argv[2],&device)),"Find device");
            Microsoft::WRL::ComPtr<IPropertyStore> saved;require(SUCCEEDED(open_saved_store(device.Get(),STGM_READ,&saved)),"Read saved EQ");
            const auto before=read_saved_profile(*saved.Get());saved.Reset();
            {
                AudioConnection connection;connection.request(argv[2],true);
                const auto until=GetTickCount64()+5000;
                while(connection.status(argv[2]).busy&&GetTickCount64()<until)Sleep(20);
                const auto check=connection.status(argv[2]);
                std::cout<<Json{{"confirmed",check.confirmed},{"busy",check.busy},{"detail",check.detail}}.dump()<<'\n';
                require(check.confirmed&&!check.busy,"No fresh Contour response to silent shared stream");
            }
            require(SUCCEEDED(open_saved_store(device.Get(),STGM_READ,&saved)),"Recheck saved EQ");
            require(read_saved_profile(*saved.Get())==before,"Silent check changed saved EQ");
            std::cout<<"PASS silent connection released; saved EQ unchanged\n";result=0;
        } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';}
        CoUninitialize();return result;
    }
    if(argc==2 && std::wstring_view(argv[1])==L"--curve-fixtures") {
        Json fixtures=Json::array();
        for(const double rate:{8000.,44100.,48000.,96000.,192000.,384000.})
        for(int type=0;type<8;++type)
        for(const double frequency:{20.,57.,1000.,rate*.45})
        for(const double q:{.05,butterworth_q,7.493,33.333,1000.})
        for(const double gain:{-18.6,0.,7.8,36.}) {
            const Filter filter{static_cast<FilterType>(type),frequency,type<3?gain:0,q};
            auto item=configuration_json({{{"Curve",{},{filter}}}},rate)["layers"][0]["filters"][0];
            fixtures.push_back({{"rate",rate},{"filter",item}});
        }
        std::cout<<fixtures.dump();return 0;
    }
    if(argc!=2||FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)))return 2;
    const auto dir=std::filesystem::path(argv[1])/(std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));std::filesystem::create_directories(dir);
    test::Suite suite;
    suite.run("desktop startup reads real profile/runtime state without writes",[&]{readonly(dir/L"startup.library");});
    suite.run("edit/gesture/undo/save bridge with DSP acknowledgement and invalid input",[&]{edits(dir/L"edits.library");});
    suite.run("native REW import, independent channels, native graph coefficients and audio",[&]{import_and_curves(dir/L"import.library");});
    suite.run("offline editing, durable partial Save and guarded exit",[&]{offline_and_exit(dir/L"offline.library");});
    suite.run("profile switch decisions, duplicate and strict bridge allowlist",[&]{profile_guard(dir/L"profiles.library");});
    suite.run("profile export/import round trip, independent global EQ, deletion guards and no device writes",[&]{profile_management(dir/L"management.library");});
    suite.run("tray preference is durable and independent of profile, draft and global EQ",[&]{tray_preferences(dir/L"tray.library");});
    suite.run("global EQ bypass preserves profiles/history and survives selection, save, restart and audio idle",[&]{master_eq(dir/L"master.library");});
    suite.run("idle edits coalesce, Save persists without playback, and close delivers automatically on resume",[&]{idle_edits(dir/L"idle.library");});
    suite.run("global EQ reaches assigned and unassigned outputs, preserves drafts and reports retryable partial failure",[&]{master_devices(dir/L"master-devices.library");});
    const auto result=suite.finish();CoUninitialize();return result;
}
