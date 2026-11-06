#include <contour/profile_device_session.h>
#include "control_test_support.h"
#include "test_support.h"
#include <fstream>
#include <iterator>

using namespace contour::eq;
using test::require;
namespace {
constexpr auto device_id=L"startup-test-output";
Configuration gain(double db) { return {{{"All",{},{},db}}}; }
Profile profile(double db) { return {gain(db),48000,2}; }
std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path,std::ios::binary); require(stream.good(),"Read library fixture");
    return {std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
}
struct Fixture {
    std::filesystem::path path;
    ProfileLibrary library;
    ProfileEntry entry;
    Microsoft::WRL::ComPtr<control_test::Store> live=Microsoft::WRL::Make<control_test::Store>();
    Microsoft::WRL::ComPtr<control_test::Store> saved=Microsoft::WRL::Make<control_test::Store>();
    Microsoft::WRL::ComPtr<control_test::Endpoint> device=Microsoft::WRL::Make<control_test::Endpoint>(live.Get(),saved.Get());
    Microsoft::WRL::ComPtr<control_test::Enumerator> enumerator=Microsoft::WRL::Make<control_test::Enumerator>(device_id,device.Get());
    SmoothEq runtime;
    std::unique_ptr<PreviewProcessor> processor;
    std::unique_ptr<ProfileDeviceSession> session;
    std::size_t channels=2;
    explicit Fixture(const std::filesystem::path& file,Profile desired=profile(-3)) : path(file),library(file),entry(library.create("Remembered",desired)) {
        ProfileWorkspace selection(library,device_id); selection.select(entry.id);
        write_saved_profile(*saved.Get(),desired); restart(gain(-3));
        session=std::make_unique<ProfileDeviceSession>(library,device_id,enumerator.Get());
    }
    std::shared_ptr<EditSession> editor() { return session->current()->editor(); }
    void restart(const Configuration& initial,double rate=48000,std::size_t count=2) {
        channels=count; processor.reset(); runtime.initialize(ResidentBank::prepare(initial,rate,count,0),static_cast<std::uint32_t>(rate*0.02));
        processor=std::make_unique<PreviewProcessor>(runtime,rate,count,std::string{},processing_fingerprint(initial,rate,count)); report();
    }
    void report() { write_control_value(*live.Get(),preview_status_key,encode_status(processor->poll())); }
    std::optional<std::string> request() { return read_control_value(*live.Get(),preview_request_key,max_request_bytes); }
    void render(double db) {
        std::vector<float> samples(2000*channels,0.1F); runtime.process(samples.data(),samples.data(),2000); report();
        test::expect_near(samples.back(),0.1*std::pow(10.0,db/20),"Inspected runtime DSP output");
    }
    void apply(double db) { const auto bytes=request(); require(bytes.has_value(),"No preview request"); processor->submit(*bytes); render(db); session->refresh(); }
};
void clean_startup(const std::filesystem::path& path) {
    Fixture f(path); const auto snapshot=f.editor()->snapshot(); const auto disk=read(path); const auto writes=f.saved->writes,commits=f.saved->commits;
    auto report=f.session->reconcile();
    require(report.library_current && report.saved==SavedSettingsState::matches && report.running_saved==RunningEqState::loaded_match && report.running_working==RunningEqState::loaded_match,"Loaded matching EQ was confused with processed audio");
    f.runtime.process(nullptr,nullptr,0); f.report(); require(f.session->reconcile().running_saved==RunningEqState::loaded_match,"Zero frames fabricated audio evidence");
    f.render(-3); report=f.session->reconcile();
    require(report.running_saved==RunningEqState::processed_match && report.running_working==RunningEqState::processed_match,"Matching processed configuration was not recognized");
    require(f.editor()->snapshot()==snapshot && !f.editor()->dirty() && !f.editor()->applied() && !f.editor()->can_undo(),"Startup inspection adopted a preview ack or changed editing state");
    require(f.saved->writes==writes && f.saved->commits==commits && !f.request() && read(path)==disk,"Read-only startup wrote settings, preview, or library");
    require(f.session->status().copy==DeviceCopyState::pending,"Inspection impersonated a successful device write");
}
void three_versions(const std::filesystem::path& path) {
    Fixture f(path); write_saved_profile(*f.saved.Get(),profile(-8)); f.restart(gain(-12)); f.render(-12);
    f.editor()->replace(gain(-12)); const auto draft=f.editor()->snapshot(); const auto disk=read(path); const auto writes=f.saved->writes;
    const auto report=f.session->reconcile();
    require(report.saved==SavedSettingsState::differs && report.windows_saved==std::optional{profile(-8)} && report.checkpoint==profile(-3),"Library and Windows saved versions were merged or lost");
    require(report.running_saved==RunningEqState::processed_difference && report.running_working==RunningEqState::processed_match,"Running EQ was inferred from saved settings");
    require(f.editor()->snapshot()==draft && f.editor()->dirty() && f.editor()->can_undo() && !f.editor()->applied(),"Inspection overwrote or acknowledged an unsent draft");
    require(f.saved->writes==writes && !f.request() && read(path)==disk,"Mismatch triggered an automatic repair");
}
void partial_save(const std::filesystem::path& path) {
    Fixture f(path); f.render(-3); f.editor()->replace(gain(-7)); f.saved->write_result=E_ACCESSDENIED;
    f.session->save(); require(!f.editor()->dirty() && f.session->status().copy==DeviceCopyState::failed,"Partial Save fixture did not preserve the library checkpoint");
    auto report=f.session->reconcile(); require(report.saved==SavedSettingsState::differs && report.running_saved==RunningEqState::processed_difference,"Interrupted Save was reported reconciled");
    f.saved->write_result=S_OK; write_saved_profile(*f.saved.Get(),profile(-7));
    report=f.session->reconcile();
    require(report.saved==SavedSettingsState::matches && report.running_saved==RunningEqState::processed_difference && f.session->status().copy==DeviceCopyState::failed,"Matching User settings hid failed Save or claimed running audio");
    const auto revision=f.library.get(f.entry.id).revision; f.session->synchronize();
    const auto request=f.request(); f.processor->submit(*request); f.report();
    require(f.session->reconcile().running_saved==RunningEqState::transitioning,"Queued retry was reported processed");
    f.render(-7); f.session->refresh(); report=f.session->reconcile();
    require(report.saved==SavedSettingsState::matches && report.running_saved==RunningEqState::processed_match && f.editor()->applied()==f.editor()->snapshot(),"Explicit retry failed to reconcile and acknowledge audio");
    require(f.library.get(f.entry.id).revision==revision,"Inspection/retry saved another library revision");
}
void bad_or_absent_saved(const std::filesystem::path& path) {
    Fixture f(path); f.restart({}); f.render(0);
    PROPVARIANT empty{}; require(SUCCEEDED(f.saved->SetValue(saved_profile_key,empty)),"Clear fixture settings");
    auto report=f.session->reconcile(); require(report.saved==SavedSettingsState::missing && report.running_saved==RunningEqState::processed_difference,"Missing saved value hid independently running neutral EQ");
    write_control_value(*f.saved.Get(),saved_profile_key,"corrupt profile");
    report=f.session->reconcile(); require(report.saved==SavedSettingsState::invalid && !report.saved_detail.empty() && report.running_saved==RunningEqState::processed_difference,"Corrupt settings were silently treated as missing or matching");
    const auto bytes=read_control_value(*f.saved.Get(),saved_profile_key,max_saved_profile_bytes); const auto writes=f.saved->writes;
    f.saved->read_result=E_ACCESSDENIED; report=f.session->reconcile();
    require(report.saved==SavedSettingsState::unreadable && report.running_saved==RunningEqState::processed_difference,"Denied access hid runtime evidence");
    f.saved->read_result=S_OK; require(f.saved->writes==writes && read_control_value(*f.saved.Get(),saved_profile_key,max_saved_profile_bytes)==bytes && !f.request(),"Inspection replaced unreadable/corrupt settings");
    write_saved_profile(*f.saved.Get(),profile(-3));
    require(SUCCEEDED(f.live->SetValue(preview_status_key,empty)),"Remove runtime fixture");
    report=f.session->reconcile(); require(report.saved==SavedSettingsState::matches && report.running_saved==RunningEqState::unavailable,"Saved EQ could not be inspected independently of an idle runtime");
    write_control_value(*f.live.Get(),preview_status_key,"corrupt status");
    report=f.session->reconcile(); require(report.saved==SavedSettingsState::matches && report.running_saved==RunningEqState::unavailable,"Malformed runtime status invented audio evidence");
}
void compatibility(const std::filesystem::path& path) {
    Fixture f(path); f.restart({},48000,4); f.render(0);
    auto report=f.session->reconcile(); require(report.saved==SavedSettingsState::matches && report.running_saved==RunningEqState::incompatible,"Channel mismatch was implicitly mapped");
    f.restart(gain(-3),44100,2); f.render(-3);
    auto high=gain(-3); high.layers[0].filters.push_back({FilterType::peak,23000,2,1}); f.editor()->replace(high);
    report=f.session->reconcile(); require(report.running_saved==RunningEqState::processed_match && report.running_working==RunningEqState::incompatible,"Working-draft compatibility was confused with the saved checkpoint");
    auto renamed=gain(-3); renamed.layers[0].name="New display name"; f.editor()->replace(renamed);
    report=f.session->reconcile(); require(report.running_saved==RunningEqState::processed_match && report.running_working==RunningEqState::processed_match && f.editor()->dirty(),"Metadata-only edits changed processing comparison or cleared dirty state");
}
void lifecycle_races(const std::filesystem::path& path) {
    Fixture f(path); f.render(-3); f.session->refresh(); const auto draft=f.editor()->snapshot(); const auto writes=f.saved->writes;
    f.device->before_activate=[&] { f.enumerator->callback->OnDeviceRemoved(device_id); f.device->device_state=DEVICE_STATE_NOTPRESENT; };
    auto report=f.session->reconcile(); require(report.saved==SavedSettingsState::unavailable && !report.runtime,"Disconnect during inspection retained a coherent-looking result");
    f.device->before_activate={}; f.device->device_state=DEVICE_STATE_ACTIVE; f.enumerator->callback->OnDeviceAdded(device_id); f.restart(gain(-3));
    report=f.session->reconcile(); require(report.saved==SavedSettingsState::matches && report.running_saved==RunningEqState::loaded_match,"Reconnected inspection did not recover without writes");
    f.device->before_activate=[&] { f.restart(gain(-8)); };
    report=f.session->reconcile(); require(report.saved==SavedSettingsState::unavailable && !report.runtime,"Runtime replacement while reading was attributed to the old observation");
    f.device->before_activate={}; f.render(-8);
    auto stale=f.processor->poll(); stale.heartbeat_ms=GetTickCount64()-control_timeout_ms-1;
    write_control_value(*f.live.Get(),preview_status_key,encode_status(stale));
    report=f.session->reconcile(); require(report.saved==SavedSettingsState::matches && report.running_saved==RunningEqState::unavailable,"Stale heartbeat retained runtime evidence");
    require(f.saved->writes==writes && !f.request() && f.editor()->snapshot()==draft && !f.editor()->applied(),"Lifecycle inspection replayed or changed the draft");
}
void stale_library(const std::filesystem::path& path) {
    Fixture f(path); f.session->refresh(); f.editor()->replace(gain(-9)); const auto draft=f.editor()->snapshot();
    ProfileLibrary external(path); bool changed=false;
    f.device->before_activate=[&] { if (!changed) { changed=true; external.rename(f.entry.id,f.entry.revision,"External name"); } };
    const auto writes=f.saved->writes;
    const auto report=f.session->reconcile();
    require(!report.library_current && report.saved==SavedSettingsState::not_checked && !report.runtime && !report.library_detail.empty(),"Library change during inspection was accepted");
    require(f.editor()->snapshot()==draft && f.editor()->dirty() && f.saved->writes==writes && !f.request(),"Library conflict replaced the current draft or device state");
}
void legacy_runtime(const std::filesystem::path& path) {
    Fixture f(path); auto legacy=f.processor->poll(); legacy.evidence=RuntimeEvidence::unknown; legacy.active_fingerprint.reset();
    auto bytes=encode_status(legacy); bytes.resize(bytes.size()-8); bytes[7]='1'; write_control_value(*f.live.Get(),preview_status_key,bytes);
    const auto report=f.session->reconcile();
    require(report.saved==SavedSettingsState::matches && report.running_saved==RunningEqState::unknown && report.running_working==RunningEqState::unknown,"Legacy runtime acquired invented configuration evidence");
    require(!f.editor()->applied() && !f.request(),"Inspecting an old runtime applied or acknowledged audio");
}
}
int wmain(int argc,wchar_t* argv[]) {
    if (argc!=2) { return 2; }
    const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED); if (FAILED(com)) { return 2; }
    const auto directory=std::filesystem::path(argv[1])/(std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(directory); test::Suite suite;
    suite.run("read-only startup, loaded versus processed, no inherited acknowledgements",[&] { clean_startup(directory/L"clean.library"); });
    suite.run("library, Windows saved value, and running EQ remain independent",[&] { three_versions(directory/L"versions.library"); });
    suite.run("interrupted Save, matching persistence without application, explicit retry",[&] { partial_save(directory/L"partial.library"); });
    suite.run("missing/corrupt/unreadable saved settings, neutral fallback, idle/corrupt runtime",[&] { bad_or_absent_saved(directory/L"missing.library"); });
    suite.run("actual channel/rate validation, unsaved draft, and metadata-independent processing",[&] { compatibility(directory/L"format.library"); });
    suite.run("disconnect/runtime change during inspection, stale heartbeat, read-only recovery",[&] { lifecycle_races(directory/L"lifecycle.library"); });
    suite.run("library changed during inspection preserves draft and refuses stale result",[&] { stale_library(directory/L"conflict.library"); });
    suite.run("legacy status remains usable with unknown running configuration",[&] { legacy_runtime(directory/L"legacy.library"); });
    const auto result=suite.finish(); CoUninitialize(); return result;
}
