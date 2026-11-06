#include "model.h"
#include "control_test_support.h"
#include "test_support.h"
#include <fstream>
#include <iostream>

using namespace contour::app;
using namespace contour::eq;
using test::require;
namespace {
constexpr auto endpoint_id=L"desktop-test-output";
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
    void consume(){
        const auto request=read_control_value(*live.Get(),preview_request_key,max_request_bytes);
        if(request)processor->submit(*request);
        std::vector<float> buffer(4000,0.1F); audio.process(buffer.data(),buffer.data(),2000); publish_status();model->poll();
    }
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
}
int wmain(int argc,wchar_t** argv){
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
    const auto result=suite.finish();CoUninitialize();return result;
}
