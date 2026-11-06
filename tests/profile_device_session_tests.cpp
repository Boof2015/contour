#include <contour/profile_device_session.h>
#include "control_test_support.h"
#include "test_support.h"
#include <array>

using namespace contour::eq;
using test::require;
namespace {
constexpr auto device_id=L"opaque-profile-device";
Configuration gain(double db) { return {{{"All",{},{},db}}}; }
Profile profile(double db) { return {gain(db),48000,2}; }
template<class F> void fails(F action) {
    bool failed=false; try { action(); } catch (const std::exception&) { failed=true; }
    require(failed,"Expected operation to fail");
}
struct Fixture {
    std::filesystem::path path;
    ProfileLibrary library;
    ProfileEntry a,b;
    Microsoft::WRL::ComPtr<control_test::Store> live=Microsoft::WRL::Make<control_test::Store>();
    Microsoft::WRL::ComPtr<control_test::Store> saved=Microsoft::WRL::Make<control_test::Store>();
    Microsoft::WRL::ComPtr<control_test::Endpoint> device=Microsoft::WRL::Make<control_test::Endpoint>(live.Get(),saved.Get());
    Microsoft::WRL::ComPtr<control_test::Enumerator> enumerator=Microsoft::WRL::Make<control_test::Enumerator>(device_id,device.Get());
    SmoothEq runtime;
    std::unique_ptr<PreviewProcessor> processor;
    ProfileDeviceSession session;
    explicit Fixture(const std::filesystem::path& file)
        : path(file),library(file),a(library.create("Studio",profile(-3))),b(library.create("Night",profile(-12))),
          session(library,device_id,enumerator.Get()) { restart(); }
    void restart(double rate=48000,std::size_t channels=2) {
        processor.reset(); runtime.initialize(ResidentBank::prepare(gain(0),rate,channels,1),static_cast<std::uint32_t>(rate*0.02));
        processor=std::make_unique<PreviewProcessor>(runtime,rate,channels); report();
    }
    void report() { write_control_value(*live.Get(),preview_status_key,encode_status(processor->poll())); }
    std::optional<std::string> request() { return read_control_value(*live.Get(),preview_request_key,max_request_bytes); }
    std::shared_ptr<EditSession> editor() { return session.current()->editor(); }
    void apply(double db) {
        const auto bytes=request(); require(bytes.has_value(),"Missing preview request");
        processor->submit(*bytes); report();
        require(session.refresh().endpoint.preview==PreviewFeedback::waiting,"Queued audio was reported applied");
        std::array<float,4000> samples{}; samples.fill(0.1F); runtime.process(samples.data(),samples.data(),2000); report();
        require(session.refresh().endpoint.preview==PreviewFeedback::applied && editor()->applied()==editor()->snapshot(),"Audio acknowledgement did not follow the current editor");
        test::expect_near(samples.back(),0.1*std::pow(10.0,db/20),"Applied profile output");
    }
};
void save_and_preview(const std::filesystem::path& path) {
    Fixture f(path);
    f.session.refresh(); require(!f.session.current() && f.saved->writes==0 && !f.request(),"Construction/refresh modified device settings");
    require(f.session.select(f.a.id)==SelectionResult::selected,"Initial selection failed");
    require(f.session.status().copy==DeviceCopyState::current && !f.editor()->applied(),"Device persistence claimed audible application");
    f.apply(-3); f.editor()->replace(gain(-7)); f.session.preview(); f.apply(-7);
    require(f.editor()->dirty() && read_saved_profile(*f.saved.Get())==std::optional{profile(-3)} && f.library.get(f.a.id).saved==profile(-3),"Preview changed a saved checkpoint");
    require(f.session.save().copy==DeviceCopyState::current && !f.editor()->dirty() && f.editor()->can_undo(),"Save did not retain history/advance the library checkpoint");
    require(read_saved_profile(*f.saved.Get())==std::optional{profile(-7)},"Save did not copy the library checkpoint to Windows");
    ProfileLibrary reopened(path); require(reopened.get(f.a.id).saved==profile(-7),"Library Save was not durable");
    f.editor()->replace(gain(-11)); f.session.preview(); f.apply(-11);
    f.session.revert(); require(!f.editor()->dirty() && !f.editor()->can_undo(),"Revert lost its library checkpoint semantics"); f.apply(-7);
    require(f.enumerator->default_queries==0,"Controller followed the Windows default device");
}
void library_failure(const std::filesystem::path& path) {
    Fixture f(path); f.session.select(f.a.id); f.apply(-3); f.editor()->replace(gain(-8));
    const auto draft=f.editor()->snapshot(),applied=f.editor()->applied();
    const auto writes=f.saved->writes,commits=f.saved->commits; const auto request=f.request();
    const auto lock=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(lock!=INVALID_HANDLE_VALUE,"Lock library fixture");
    fails([&] { f.session.save(); }); fails([&] { f.session.select(f.b.id,UnsavedChoice::save); }); fails([&] { f.session.save_as("Copy"); });
    CloseHandle(lock);
    require(f.session.current()->id()==f.a.id && f.editor()->snapshot()==draft && f.editor()->dirty() && f.editor()->applied()==applied,"Failed library persistence altered editor state");
    require(f.saved->writes==writes && f.saved->commits==commits && f.request()==request,"Failed library Save touched the device");
    f.session.save(); f.apply(-8);
}
void partial_save_and_retry(const std::filesystem::path& path) {
    Fixture f(path); f.session.select(f.a.id); f.apply(-3); f.editor()->replace(gain(-9));
    const auto request=f.request(); f.saved->commit_result=E_FAIL;
    require(f.session.save().copy==DeviceCopyState::failed,"Failed Commit reported the device copy current");
    require(!f.editor()->dirty() && f.library.get(f.a.id).saved==profile(-9) && f.editor()->can_undo(),"Device failure rolled back or lost the durable library Save");
    ProfileLibrary reopened(path); require(reopened.get(f.a.id).saved==profile(-9),"Partial Save was not recoverable from the library");
    require(!f.session.status().copy_detail.empty() && f.request()==request,"Failed copy silently published a new preview");
    const auto writes=f.saved->writes;
    f.saved->commit_result=S_OK; f.session.refresh();
    require(f.session.status().copy==DeviceCopyState::failed && f.saved->writes==writes,"Refresh hid an uncertain Commit or retried it automatically");
    require(f.session.synchronize().copy==DeviceCopyState::current,"Explicit retry did not recover"); f.apply(-9);
    f.editor()->replace(gain(-5)); f.saved->discard_writes=true;
    require(f.session.save().copy==DeviceCopyState::failed && !f.editor()->dirty(),"Readback mismatch was reported as a complete Save");
    f.saved->discard_writes=false; f.session.synchronize(); f.apply(-5);
    f.editor()->replace(gain(-4)); f.saved->write_result=E_ACCESSDENIED;
    require(f.session.save().copy==DeviceCopyState::failed && f.library.get(f.a.id).saved==profile(-4),"Denied device access lost library edits");
}
void switching(const std::filesystem::path& path) {
    Fixture f(path); f.session.select(f.a.id); f.apply(-3); const auto old=f.editor();
    old->replace(gain(-8)); f.session.preview(); f.apply(-8);
    const auto request=f.request(); const auto writes=f.saved->writes;
    require(f.session.select(f.b.id)==SelectionResult::needs_decision && f.session.select(f.b.id,UnsavedChoice::cancel)==SelectionResult::cancelled,"Dirty selection skipped its guard");
    fails([&] { f.session.select("missing",UnsavedChoice::save); });
    require(f.request()==request && f.saved->writes==writes && old->applied()==old->snapshot(),"Cancelled/invalid selection changed audio");
    require(f.session.select(f.b.id,UnsavedChoice::discard)==SelectionResult::selected,"Discard switch failed");
    require(!old->dirty() && !old->can_undo() && !old->applied() && !f.editor()->applied(),"Switch retained discarded edits or transferred old audio confirmation");
    require(f.session.refresh().endpoint.preview==PreviewFeedback::waiting,"Old profile acknowledgement confirmed the new profile"); f.apply(-12);
    f.editor()->replace(gain(-14)); f.session.select(f.a.id,UnsavedChoice::save);
    require(f.library.get(f.b.id).saved==profile(-14) && read_saved_profile(*f.saved.Get())==std::optional{profile(-3)},"Save-and-switch persisted the wrong profile to the device"); f.apply(-3);
    require(f.enumerator->registrations==1 && f.enumerator->unregistrations==0,"Switch replaced the device lifecycle observer");
}
void save_as(const std::filesystem::path& path) {
    Fixture f(path); f.session.select(f.a.id); f.apply(-3); const auto source=f.session.current(); const auto old=f.editor();
    old->replace(gain(-6)); f.session.preview(); f.apply(-6);
    const auto writes=f.saved->writes; const auto request=f.request();
    f.session.rename("New name"); const auto duplicate=f.session.duplicate("Independent copy");
    require(f.saved->writes==writes && f.request()==request && duplicate.saved==profile(-6),"Rename/duplicate changed device settings");
    const auto copy=f.session.save_as("New identity");
    require(copy.id==f.session.current()->id() && !f.editor()->dirty() && f.editor()!=old && !old->applied(),"Save as did not detach the old editor safely");
    require(source->editor()->working()==profile(-3) && f.library.get(f.a.id).saved==profile(-3) && !source->editor()->can_undo(),"Save as altered the source checkpoint"); f.apply(-6);
    old->replace(gain(-20)); f.session.preview(); // A retained, detached handle must not drive the current endpoint.
    require(parse_profile(decode_request(*f.request()).profile)==profile(-6),"Detached source editor changed the current preview");
    require(f.enumerator->registrations==1,"Save as discarded lifecycle history");
}
void reconnect_and_format(const std::filesystem::path& path) {
    Fixture f(path); f.session.select(f.a.id); f.apply(-3);
    f.enumerator->callback->OnDeviceRemoved(device_id); f.enumerator->lookup_result=E_NOTFOUND;
    require(f.session.refresh().copy==DeviceCopyState::pending && !f.editor()->applied(),"Removal retained confirmation");
    f.editor()->replace(gain(-6)); f.session.save();
    require(!f.editor()->dirty() && f.library.get(f.a.id).saved==profile(-6),"Offline Save lost edits");
    const auto writes=f.saved->writes;
    f.enumerator->lookup_result=S_OK; f.enumerator->callback->OnDeviceAdded(device_id);
    f.session.select(f.b.id);
    require(f.session.status().endpoint.state==EndpointState::waiting_for_audio && !f.editor()->applied(),"Profile switch revived the retired runtime after a quick replug");
    require(f.saved->writes>writes && read_saved_profile(*f.saved.Get())==std::optional{f.library.get(f.b.id).saved},"Idle selection did not persist its checkpoint");
    const auto idle_writes=f.saved->writes;
    f.restart(); f.session.refresh();
    require(f.saved->writes==idle_writes && !f.editor()->applied(),"Read-only refresh rewrote settings or claimed application");
    f.session.synchronize(); f.apply(-12);
    const auto surround=f.library.create("Four channels",{gain(-4),48000,4}); const auto before=f.saved->writes; const auto request=f.request();
    f.session.select(surround.id);
    require(f.session.status().copy==DeviceCopyState::failed && f.session.status().endpoint.state==EndpointState::incompatible,"Layout mismatch was silently applied");
    require(f.saved->writes==before && f.request()==request,"Incompatible selection touched the device");
    f.session.select(f.a.id); f.apply(-6);
    auto high=gain(-7); high.layers[0].filters.push_back({FilterType::peak,23000,-2,1}); f.editor()->replace(high);
    f.restart(44100); f.session.synchronize();
    require(f.session.status().copy==DeviceCopyState::current && !f.session.status().preview_detail.empty() && f.editor()->dirty(),"Saved compatible checkpoint was confused with an incompatible working draft");
    require(read_saved_profile(*f.saved.Get())==std::optional{profile(-6)},"Synchronize persisted the unsaved draft");
    f.session.save();
    require(!f.editor()->dirty() && f.session.status().copy==DeviceCopyState::failed,"Library-valid, device-invalid Save lost edits or falsely applied them");
}
void notification_during_save(const std::filesystem::path& path) {
    Fixture f(path); f.session.select(f.a.id); f.apply(-3); f.editor()->replace(gain(-10));
    const auto request=f.request();
    f.saved->after_commit=[&] { f.enumerator->callback->OnDeviceRemoved(device_id); };
    require(f.session.save().copy==DeviceCopyState::failed && !f.editor()->dirty(),"Removal during Commit falsely confirmed both stores");
    require(f.library.get(f.a.id).saved==profile(-10) && f.request()==request && !f.editor()->applied(),"Removal during Save lost the library checkpoint or submitted to an old graph");
    f.saved->after_commit={}; f.restart(); f.session.synchronize(); f.apply(-10);
    require(f.session.status().copy==DeviceCopyState::current,"Retry after graph replacement did not recover");
    // The copy completes in one runtime, but the very next refresh sees a new
    // runtime. Its identity must not be substituted into the old copy receipt.
    f.editor()->replace(gain(-8)); const auto prior_request=f.request(); bool replace_on_lookup=false;
    f.saved->after_commit=[&] { replace_on_lookup=true; };
    f.enumerator->before_lookup=[&] { if (replace_on_lookup) { replace_on_lookup=false; f.restart(); } };
    require(f.session.save().copy==DeviceCopyState::failed && !f.editor()->dirty(),"Copy receipt was attributed to a replacement runtime");
    require(f.request()==prior_request,"Runtime changed after copying but still received a preview");
    f.saved->after_commit={}; f.enumerator->before_lookup={}; f.session.synchronize(); f.apply(-8);
}
void restored_selection(const std::filesystem::path& path) {
    Fixture f(path); f.session.select(f.a.id); f.apply(-3); f.editor()->replace(gain(-8));
    const auto old_draft=f.editor()->snapshot(),old_applied=f.editor()->applied();
    const auto writes=f.saved->writes; const auto request=f.request();
    ProfileLibrary reopened(path);
    auto enumerator=Microsoft::WRL::Make<control_test::Enumerator>(device_id,f.device.Get());
    {
        ProfileDeviceSession restored(reopened,device_id,enumerator.Get());
        require(restored.current() && restored.current()->id()==f.a.id && restored.current()->editor()->working()==profile(-3),"Reopening did not restore the assigned saved profile");
        require(enumerator->lookups==0 && f.saved->writes==writes && f.request()==request,"Construction touched a real device or republished an old request");
        restored.refresh();
        require(restored.status().copy==DeviceCopyState::pending && !restored.current()->editor()->applied() && !restored.current()->editor()->dirty(),"Restore inherited copy/audio confirmation or an unsaved draft");
        require(f.saved->writes==writes && f.request()==request && f.editor()->snapshot()==old_draft && f.editor()->applied()==old_applied,"Read-only restore changed audio or another editor");
        require(restored.select(f.a.id)==SelectionResult::unchanged && f.saved->writes==writes,"Reselecting restored profile became an implicit synchronization");
        require(restored.synchronize().copy==DeviceCopyState::current,"Restored assignment could not be explicitly applied");
        require(restored.refresh().endpoint.preview==PreviewFeedback::waiting,"Prior session acknowledgement confirmed the restored session");
        f.processor->submit(*f.request()); std::array<float,4000> samples{}; samples.fill(0.1F); f.runtime.process(samples.data(),samples.data(),2000); f.report();
        require(restored.refresh().endpoint.preview==PreviewFeedback::applied,"Restored session did not acknowledge its own preview");
        test::expect_near(samples.back(),0.1*std::pow(10.0,-3.0/20),"Restored profile DSP output");
    }
    require(enumerator->callback==nullptr && enumerator->registrations==enumerator->unregistrations,"Restored controller leaked its device observer");
    ProfileDeviceSession other(reopened,L"unassigned endpoint",enumerator.Get());
    require(!other.current() && other.endpoint_id()!=device_id,"Unknown endpoint inherited the previous/default device assignment");
}
void assignment_commit_failure(const std::filesystem::path& path) {
    Fixture f(path); f.session.select(f.a.id); f.apply(-3); f.editor()->replace(gain(-8));
    const auto snapshot=f.editor()->snapshot(); const auto assignment=f.library.assignment(device_id); const auto writes=f.saved->writes; const auto request=f.request();
    const auto lock=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(lock!=INVALID_HANDLE_VALUE,"Lock assignment fixture");
    fails([&] { f.session.select(f.b.id,UnsavedChoice::discard); }); fails([&] { f.session.select(f.b.id,UnsavedChoice::save); }); fails([&] { f.session.save_as("New"); });
    CloseHandle(lock);
    require(f.library.assignment(device_id)==assignment && f.library.get(f.a.id).saved==profile(-3) && f.library.entries().size()==2,"Assignment failure partly saved profile metadata");
    require(f.editor()->snapshot()==snapshot && f.editor()->dirty() && f.saved->writes==writes && f.request()==request,"Assignment failure discarded edits or reached the device");
    f.saved->commit_result=E_FAIL;
    const auto created=f.session.save_as("Offline copy");
    require(f.session.status().copy==DeviceCopyState::failed && f.library.assignment(device_id)->profile_id==created.id,"Device failure rolled back the durable desired assignment");
    ProfileLibrary reopened(path); auto enumerator=Microsoft::WRL::Make<control_test::Enumerator>(device_id,f.device.Get());
    ProfileDeviceSession restored(reopened,device_id,enumerator.Get());
    require(restored.current()->id()==created.id && restored.current()->editor()->saved()==profile(-8),"Restart after partial Save as lost its desired profile");
    enumerator->lookup_result=E_NOTFOUND; const auto prior_writes=f.saved->writes; restored.refresh();
    require(restored.status().endpoint.state==EndpointState::unavailable && restored.current()->id()==created.id && f.saved->writes==prior_writes,"Offline restore forgot the assignment or wrote device settings");
    enumerator->lookup_result=S_OK; f.saved->commit_result=S_OK; f.restart(); restored.refresh();
    require(f.saved->writes==prior_writes && !restored.current()->editor()->applied(),"Reconnect replayed the restored profile automatically");
    require(restored.synchronize().copy==DeviceCopyState::current && read_saved_profile(*f.saved.Get())==std::optional{profile(-8)},"Restored partial Save as could not be retried");
}
void changed_assignment(const std::filesystem::path& path) {
    Fixture f(path); f.session.select(f.a.id); f.apply(-3); f.editor()->replace(gain(-8));
    ProfileLibrary external(path); ProfileWorkspace newer(external,device_id); newer.select(f.b.id);
    const auto writes=f.saved->writes; const auto request=f.request();
    f.session.preview(); require(!f.session.status().preview_detail.empty() && f.request()==request,"Stale catalog preview overwrote a newer device choice");
    require(f.session.synchronize().copy==DeviceCopyState::failed && f.saved->writes==writes,"Stale assignment wrote endpoint saved settings");
    f.library.reload(); f.session.preview(); fails([&] { f.session.save(); }); fails([&] { f.session.select(f.b.id,UnsavedChoice::discard); });
    require(f.editor()->dirty() && f.session.current()->id()==f.a.id && f.saved->writes==writes && f.request()==request && f.library.assignment(device_id)->profile_id==f.b.id,"Reload silently replaced a draft or permitted a stale controller to take ownership");
}
}
int wmain(int argc,wchar_t* argv[]) {
    if (argc!=2) { return 2; }
    const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED); if (FAILED(com)) { return 2; }
    const auto directory=std::filesystem::path(argv[1])/(std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(directory); test::Suite suite;
    suite.run("library Save, independent draft preview, persistence/acknowledgement, and Revert",[&] { save_and_preview(directory/L"save.library"); });
    suite.run("failed library Save/switch/Save as never touch device state",[&] { library_failure(directory/L"blocked.library"); });
    suite.run("partial Save, failed Commit/readback/access, durable checkpoint, explicit retry",[&] { partial_save_and_retry(directory/L"retry.library"); });
    suite.run("guarded switches, source Save/discard, new preview identity, retained observer",[&] { switching(directory/L"switch.library"); });
    suite.run("metadata isolation, Duplicate, Save as, and detached editor lifetime",[&] { save_as(directory/L"copy.library"); });
    suite.run("offline Save, quick replug plus profile switch, channel/rate mismatch, draft versus saved copy",[&] { reconnect_and_format(directory/L"reconnect.library"); });
    suite.run("device notification during Commit preserves library and rejects stale graph",[&] { notification_during_save(directory/L"race.library"); });
    suite.run("remembered selection restores read-only, no draft/ack leakage, explicit DSP application",[&] { restored_selection(directory/L"restore.library"); });
    suite.run("failed assignment transaction and restart/retry after partial device Save as",[&] { assignment_commit_failure(directory/L"assignment-failure.library"); });
    suite.run("stale assignment/catalog blocks device writes without replacing unsaved edits",[&] { changed_assignment(directory/L"assignment-conflict.library"); });
    const auto result=suite.finish(); CoUninitialize(); return result;
}
