#include <contour/profile_device_session.h>
#include "control_test_support.h"
#include "test_support.h"
#include <array>

using namespace contour::eq;
using test::require;
namespace {
constexpr auto device_id=L"exit-test-output";
Configuration gain(double db) { return {{{"All",{},{},db}}}; }
Profile profile(double db) { return {gain(db),48000,2}; }
template<class F> void fails(F action) {
    bool failed=false; try { action(); } catch (const std::exception&) { failed=true; }
    require(failed,"Expected rejected operation");
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
    explicit Fixture(const std::filesystem::path& file,bool select=true)
        : path(file),library(file),a(library.create("Studio",profile(-3))),b(library.create("Night",profile(-12))),session(library,device_id,enumerator.Get()) {
        restart(); if (select) { session.select(a.id); apply(-3); }
    }
    std::shared_ptr<EditSession> editor() { return session.current()->editor(); }
    void report() { write_control_value(*live.Get(),preview_status_key,encode_status(processor->poll())); }
    void restart(double rate=48000) {
        processor.reset(); runtime.initialize(ResidentBank::prepare(gain(0),rate,2,1),static_cast<std::uint32_t>(rate*0.02));
        processor=std::make_unique<PreviewProcessor>(runtime,rate,2); report();
    }
    std::optional<std::string> request() { return read_control_value(*live.Get(),preview_request_key,max_request_bytes); }
    void queue() { const auto bytes=request(); require(bytes.has_value(),"No audio request"); processor->submit(*bytes); report(); }
    void render(double db) {
        std::array<float,4000> samples{}; samples.fill(0.1F); runtime.process(samples.data(),samples.data(),2000); report();
        test::expect_near(samples.back(),0.1*std::pow(10.0,db/20),"Exit target DSP output");
    }
    void apply(double db) { queue(); render(db); session.refresh(); }
    void audition(double db) { editor()->replace(gain(db)); session.preview(); apply(db); }
};
void guard_and_cancel(const std::filesystem::path& path) {
    Fixture f(path); f.audition(-9); const auto editor=f.editor();
    const auto snapshot=editor->snapshot(),applied=editor->applied(); const auto request=f.request(); const auto writes=f.saved->writes,live_writes=f.live->writes;
    require(f.session.request_close().state==CloseState::needs_decision,"Dirty close skipped its decision");
    require(f.session.finish_close(CloseCompletion::leave_device_pending).state==CloseState::needs_decision,"Pending-device completion bypassed an unresolved dirty decision");
    require(f.session.request_close(UnsavedChoice::cancel).state==CloseState::open,"Cancel did not reopen editor");
    require(f.editor()==editor && editor->snapshot()==snapshot && editor->applied()==applied && editor->dirty() && editor->can_undo(),"Cancel changed the draft/checkpoint/history/audio claim");
    require(f.saved->writes==writes && f.live->writes==live_writes && f.request()==request,"Prompt or Cancel wrote device settings");
    fails([&] { f.session.request_close(static_cast<UnsavedChoice>(99)); });
    require(f.session.close_status().state==CloseState::open,"Invalid decision changed close state");
}
void discard_and_ack(const std::filesystem::path& path) {
    Fixture f(path); f.audition(-9); const auto editor=f.editor(); const auto draft=editor->snapshot(); const auto assignment=f.library.assignment(device_id);
    require(f.session.request_close(UnsavedChoice::discard).state==CloseState::waiting,"Discard did not wait for saved audio");
    require(editor->snapshot()==draft && editor->dirty() && editor->can_undo() && !editor->applied(),"Discard destroyed the draft before close completed");
    require(f.library.get(f.a.id).saved==profile(-3) && read_saved_profile(*f.saved.Get())==std::optional{profile(-3)},"Discard saved the unsaved draft");
    require(parse_profile(decode_request(*f.request()).profile)==profile(-3),"Discard requested the wrong EQ");
    require(f.session.finish_close().state==CloseState::waiting && f.session.current(),"Old audio acknowledgement was accepted for exit");
    f.queue(); require(f.session.poll_close().state==CloseState::waiting,"Queued restoration was reported complete");
    f.render(-3); require(f.session.poll_close().state==CloseState::ready && editor->dirty(),"Confirmed audio prematurely destroyed the draft");
    require(f.session.finish_close().state==CloseState::closed && !f.session.current() && !f.session.close_status().device_pending,"Confirmed discard did not close");
    require(editor->working()==profile(-3) && !editor->dirty() && !editor->can_undo() && f.library.assignment(device_id)==assignment,"Final discard lost the checkpoint or remembered assignment");
    require(f.enumerator->callback==nullptr && f.enumerator->registrations==f.enumerator->unregistrations,"Closing leaked the endpoint callback");
    const auto lookups=f.enumerator->lookups,writes=f.saved->writes;
    f.session.refresh(); f.session.finish_close(); require(f.enumerator->lookups==lookups && f.saved->writes==writes,"Closed controller reopened the device");
    fails([&] { f.session.select(f.b.id); }); fails([&] { f.session.preview(); });
}
void saved_close(const std::filesystem::path& path) {
    Fixture f(path); f.editor()->replace(gain(-7)); const auto editor=f.editor();
    require(f.session.request_close(UnsavedChoice::save).state==CloseState::waiting,"Save close did not wait for audio");
    require(!editor->dirty() && editor->can_undo() && f.library.get(f.a.id).saved==profile(-7),"Save close lost the checkpoint or undo history");
    const auto revision=f.library.get(f.a.id).revision; const auto writes=f.saved->writes; const auto request=f.request();
    f.session.poll_close(); f.session.request_close(UnsavedChoice::save);
    require(f.library.get(f.a.id).revision==revision && f.saved->writes==writes && f.request()==request,"Polling/repeated close repeated Save or publication");
    f.apply(-7); require(f.session.finish_close().state==CloseState::closed,"Save close did not accept the confirmed target");
    ProfileLibrary reopened(path); require(reopened.get(f.a.id).saved==profile(-7) && reopened.assignment(device_id)->profile_id==f.a.id,"Saved close was not durable");
    Fixture clean(std::filesystem::path(path.wstring()+L".clean")); const auto clean_writes=clean.saved->writes; const auto clean_request=clean.request();
    require(clean.session.request_close().state==CloseState::ready && clean.saved->writes==clean_writes && clean.request()==clean_request,"Already confirmed clean close rewrote device state");
    require(clean.session.finish_close().state==CloseState::closed,"Clean close failed");
    Fixture cancelled(std::filesystem::path(path.wstring()+L".cancelled")); cancelled.editor()->replace(gain(-5));
    cancelled.session.request_close(UnsavedChoice::save); const auto saved_revision=cancelled.library.get(cancelled.a.id).revision;
    require(cancelled.session.cancel_close().state==CloseState::open && !cancelled.editor()->dirty() && cancelled.editor()->can_undo(),"Cancelling after Save reversed the durable checkpoint or lost history");
    cancelled.session.request_close(); cancelled.apply(-5); cancelled.session.finish_close();
    require(cancelled.library.get(cancelled.a.id).revision==saved_revision,"Reclosing after Save cancellation saved the library again");
}
void failed_save_and_retry(const std::filesystem::path& path) {
    Fixture f(path); f.audition(-8); const auto editor=f.editor(); const auto draft=editor->snapshot(); const auto applied=editor->applied();
    const auto writes=f.saved->writes; const auto request=f.request();
    const auto lock=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(lock!=INVALID_HANDLE_VALUE,"Lock library fixture");
    const auto blocked=f.session.request_close(UnsavedChoice::save).state; CloseHandle(lock);
    require(blocked==CloseState::needs_attention && editor->snapshot()==draft && editor->dirty() && editor->applied()==applied,"Failed library Save lost edits or confirmation");
    require(f.saved->writes==writes && f.request()==request && f.library.get(f.a.id).saved==profile(-3),"Failed library Save reached the device");
    require(f.session.finish_close(CloseCompletion::leave_device_pending).state==CloseState::needs_attention && f.session.current(),"Quit-pending bypassed failed library Save");
    f.saved->commit_result=E_FAIL;
    require(f.session.request_close(UnsavedChoice::save).state==CloseState::needs_attention && !editor->dirty(),"Device Commit failure was hidden or lost the library Save");
    const auto revision=f.library.get(f.a.id).revision;
    f.saved->commit_result=S_OK;
    require(f.session.retry_close().state==CloseState::waiting && f.library.get(f.a.id).revision==revision,"Retry saved a new library revision or failed to publish");
    f.apply(-8); require(f.session.finish_close().state==CloseState::closed,"Retry after partial Save did not close");
}
void device_loss(const std::filesystem::path& path) {
    Fixture f(path); f.audition(-10); const auto editor=f.editor(); const auto draft=editor->snapshot();
    f.session.request_close(UnsavedChoice::discard); f.enumerator->callback->OnDeviceRemoved(device_id); f.enumerator->lookup_result=E_NOTFOUND;
    require(f.session.poll_close().state==CloseState::needs_attention && editor->snapshot()==draft && editor->dirty(),"Device loss discarded the draft or falsely confirmed exit");
    require(f.session.finish_close().state==CloseState::needs_attention,"Device failure automatically allowed exit");
    const auto writes=f.saved->writes;
    f.enumerator->lookup_result=S_OK; f.enumerator->callback->OnDeviceAdded(device_id); f.restart(); f.session.refresh(); f.session.poll_close();
    require(f.saved->writes==writes && f.session.close_status().state==CloseState::needs_attention,"Reconnect retried a pending close automatically");
    require(f.session.retry_close().state==CloseState::waiting,"Explicit reconnect retry did not restart close");
    f.apply(-3); require(f.session.poll_close().state==CloseState::ready,"Reconnected discard was not ready");
    // Readiness is not a permanent permission to close: recheck at final handoff.
    f.enumerator->callback->OnDeviceRemoved(device_id); f.enumerator->lookup_result=E_NOTFOUND;
    require(f.session.finish_close().state==CloseState::needs_attention && editor->dirty(),"Final handoff trusted readiness from a disappeared device");
    f.enumerator->lookup_result=S_OK; f.restart(); f.session.retry_close(); f.apply(-3);
    require(f.session.finish_close().state==CloseState::closed && !editor->dirty(),"Reconnected discard did not finish");
    Fixture pending(std::filesystem::path(path.wstring()+L".pending")); pending.audition(-11); const auto pending_editor=pending.editor();
    pending.enumerator->lookup_result=E_NOTFOUND;
    require(pending.session.request_close(UnsavedChoice::discard).state==CloseState::needs_attention,"Unavailable device was reported restored");
    require(pending.session.finish_close(CloseCompletion::leave_device_pending).state==CloseState::closed && pending.session.close_status().device_pending,"Explicit quit-pending was not distinguished from confirmed restoration");
    require(!pending_editor->dirty() && pending_editor->working()==profile(-3) && pending.library.get(pending.a.id).saved==profile(-3),"Quit-pending persisted discarded edits");
}
void timeout_and_cancel(const std::filesystem::path& path) {
    Fixture f(path); f.audition(-9); const auto editor=f.editor(); const auto draft=editor->snapshot();
    f.session.request_close(UnsavedChoice::discard);
    const auto writes=f.saved->writes; const auto request=f.request();
    // Heartbeat remains live, but no audio callback consumes the final request.
    Sleep(static_cast<DWORD>(ProfileDeviceSession::close_timeout_ms+30)); f.report();
    require(f.session.poll_close().state==CloseState::needs_attention && !f.session.close_status().detail.empty(),"Missing audio callbacks left exit waiting forever or falsely confirmed it");
    require(f.session.finish_close().state==CloseState::needs_attention,"Timeout silently allowed close");
    require(f.session.cancel_close().state==CloseState::open && editor->snapshot()==draft && editor->dirty() && editor->can_undo(),"Cancel during restoration lost the draft/history");
    require(f.saved->writes==writes && f.request()==request,"Cancel unexpectedly published another EQ");
    f.apply(-3); require(!editor->applied(),"Late exit acknowledgement confirmed the restored working editor");
    f.session.preview(); f.apply(-9); require(editor->applied()==editor->snapshot(),"Retained draft could not be auditioned again");
}
void changed_draft_and_owner(const std::filesystem::path& path) {
    Fixture f(path); f.audition(-8); const auto editor=f.editor(); f.session.request_close(UnsavedChoice::discard);
    fails([&] { f.session.save(); }); fails([&] { f.session.rename("Changed"); }); fails([&] { f.session.select(f.b.id); });
    editor->replace(gain(-11)); // Simulate an already queued UI edit using a retained handle.
    require(f.session.poll_close().state==CloseState::needs_attention,"A changed draft did not invalidate the exit decision");
    require(f.session.finish_close(CloseCompletion::leave_device_pending).state==CloseState::needs_attention && editor->working()==profile(-11),"Quit-pending discarded edits made after the decision");
    f.session.cancel_close(); f.session.request_close(UnsavedChoice::discard);
    ProfileLibrary external(path); ProfileWorkspace owner(external,device_id); owner.select(f.b.id);
    const auto writes=f.saved->writes; const auto request=f.request();
    require(f.session.poll_close().state==CloseState::needs_attention && f.saved->writes==writes && f.request()==request,"Close ignored a newer device assignment");
    f.session.cancel_close(); require(editor->dirty() && editor->working()==profile(-11),"Assignment conflict destroyed the retained draft");
}
void readonly_exit(const std::filesystem::path& path) {
    Fixture empty(path,false);
    require(empty.session.request_close().state==CloseState::ready && empty.session.finish_close().state==CloseState::closed,"Unassigned session could not close");
    require(empty.enumerator->lookups==0 && empty.saved->writes==0 && !empty.request(),"Unassigned close contacted the device");
    ProfileWorkspace assign(empty.library,device_id); assign.select(empty.a.id);
    auto enumerator=Microsoft::WRL::Make<control_test::Enumerator>(device_id,empty.device.Get()); enumerator->lookup_result=E_NOTFOUND;
    ProfileDeviceSession restored(empty.library,device_id,enumerator.Get()); const auto editor=restored.current()->editor();
    editor->replace(gain(-7));
    require(restored.request_close().state==CloseState::needs_decision,"Unpreviewed edits skipped the close guard");
    require(restored.request_close(UnsavedChoice::discard).state==CloseState::ready && editor->dirty(),"Read-only discard depended on an audio runtime or erased edits early");
    require(restored.finish_close().state==CloseState::closed && !editor->dirty() && !editor->can_undo(),"Read-only discard did not complete");
    require(enumerator->lookups==0 && empty.saved->writes==0 && !empty.request(),"Read-only exit attempted device reconciliation");
}
}
int wmain(int argc,wchar_t* argv[]) {
    if (argc!=2) { return 2; }
    const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED); if (FAILED(com)) { return 2; }
    const auto directory=std::filesystem::path(argv[1])/(std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(directory); test::Suite suite;
    suite.run("dirty guard, Cancel, invalid choices, and zero device writes",[&] { guard_and_cancel(directory/L"guard.library"); });
    suite.run("Don't save retains draft until final audio ack, measured restoration, clean final release",[&] { discard_and_ack(directory/L"discard.library"); });
    suite.run("Save close persists once and waits for audio; confirmed clean close writes nothing",[&] { saved_close(directory/L"save.library"); });
    suite.run("failed library Save cannot be bypassed; partial device Save retry retains one checkpoint",[&] { failed_save_and_retry(directory/L"failure.library"); });
    suite.run("disconnect while closing, explicit replug retry, and explicit quit with device pending",[&] { device_loss(directory/L"disconnect.library"); });
    suite.run("bounded nonblocking wait, cancelled restoration, late ack isolation, retained draft audition",[&] { timeout_and_cancel(directory/L"timeout.library"); });
    suite.run("queued UI edits and external assignment changes invalidate close without losing edits",[&] { changed_draft_and_owner(directory/L"changed.library"); });
    suite.run("unassigned and read-only offline exit do not require audio or write device settings",[&] { readonly_exit(directory/L"readonly.library"); });
    const auto result=suite.finish(); CoUninitialize(); return result;
}
