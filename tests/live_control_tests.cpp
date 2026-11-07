#include <contour/live_control.h>
#include "control_test_support.h"
#include "test_support.h"
#include <array>
#include <limits>

using namespace contour::eq;
using test::require;
namespace {
template<class F> void rejects(F action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Expected invalid_argument");
}
Configuration gain(double db) { return {{{"All",{},{},db}}}; }
GUID id() { GUID value{}; require(SUCCEEDED(CoCreateGuid(&value)),"Create fixture identity"); return value; }
void roundtrips() {
    const EngineIdentity engine{id(),L"C:\\Program Files\\Contour\\runtimes\\test\\contour-apo.dll"};
    const auto engine_bytes=encode_engine_identity(engine);
    require(decode_engine_identity(engine_bytes).runtime==engine.runtime&&decode_engine_identity(engine_bytes).module_path==engine.module_path,"Engine identity lost loaded path or runtime");
    for(std::size_t i=0;i<engine_bytes.size();++i)rejects([&]{decode_engine_identity(std::string_view(engine_bytes).substr(0,i));});
    rejects([&]{decode_engine_identity(engine_bytes+"x");});
    rejects([&]{encode_engine_identity({GUID_NULL,engine.module_path});});
    rejects([&]{encode_engine_identity({engine.runtime,L""});});
    const PreviewRequest request{id(),id(),std::numeric_limits<std::uint64_t>::max(),serialize_profile({gain(-3),48000,2})};
    const auto bytes=encode_request(request);
    const auto decoded=decode_request(bytes);
    require(decoded.runtime==request.runtime && decoded.session==request.session && decoded.revision==request.revision &&
            decoded.profile==request.profile,"request lost identity, revision, or precision");
    for (std::size_t i=0; i<bytes.size(); ++i) { rejects([&] { decode_request(std::string_view(bytes).substr(0,i)); }); }
    rejects([&] { decode_request(bytes+"x"); });
    auto bad=bytes; bad[7]='2'; rejects([&] { decode_request(bad); });
    rejects([&] { decode_request(std::string(max_request_bytes+1,'x')); });
    auto invalid=request; invalid.revision=0; rejects([&] { encode_request(invalid); });
    invalid=request; invalid.runtime=GUID_NULL; rejects([&] { encode_request(invalid); });
    PreviewStatus status{request.runtime,request.session,request.revision,PreviewState::rejected,44100,2,10000,"Invalid frequency"};
    const auto encoded=encode_status(status);
    require(encode_status(decode_status(encoded))==encoded,"status lost fields");
    for (std::size_t i=0; i<encoded.size(); ++i) { rejects([&] { decode_status(std::string_view(encoded).substr(0,i)); }); }
    rejects([&] { decode_status(encoded+"x"); });
    rejects([&] { decode_status(std::string(max_status_bytes+1,'x')); });
    auto legacy=encoded.substr(0,encoded.size()-8); legacy[7]='1';
    require(decode_status(legacy).evidence==RuntimeEvidence::unknown && !decode_status(legacy).active_fingerprint,"Legacy status invented processing evidence");
    status.evidence=RuntimeEvidence::processed; status.active_fingerprint=processing_fingerprint(gain(-3),44100,2);
    const auto observed=encode_status(status); require(decode_status(observed).active_fingerprint==status.active_fingerprint,"Fingerprint roundtrip lost data");
    for (std::size_t i=0;i<observed.size();++i) { rejects([&] { decode_status(std::string_view(observed).substr(0,i)); }); }
    auto bad_status=observed; bad_status[7]='3'; rejects([&] { decode_status(bad_status); });
    bad_status=observed; bad_status[bad_status.size()-36]=2; rejects([&] { decode_status(bad_status); });
    bad_status=observed; bad_status[bad_status.size()-40]=99; rejects([&] { decode_status(bad_status); });
    status.active_fingerprint.reset(); rejects([&] { encode_status(status); }); status.evidence=RuntimeEvidence::unknown;
    status.state=static_cast<PreviewState>(99); rejects([&] { encode_status(status); });
    status.state=PreviewState::ready; status.sample_rate=std::numeric_limits<double>::quiet_NaN(); rejects([&] { encode_status(status); });
}
void server_validation() {
    SmoothEq runtime; runtime.initialize(ResidentBank::prepare(gain(-3),44100,2,1),882);
    PreviewProcessor processor(runtime,44100,2);
    const auto epoch=processor.poll().runtime;
    const auto session=id();
    PreviewRequest request{epoch,session,1,serialize_profile({gain(-9),48000,2})};
    processor.submit(encode_request(request));
    require(processor.poll().state==PreviewState::queued,"enqueue reported applied");
    std::array<float,2000> samples{}; samples.fill(0.1F);
    runtime.process(samples.data(),samples.data(),20);
    require(processor.poll().state==PreviewState::queued,"partial crossfade reported applied");
    samples.fill(0.1F); runtime.process(samples.data(),samples.data(),1000);
    require(processor.poll().state==PreviewState::applied,"completed crossfade not acknowledged");
    test::expect_near(samples.back(),0.1*std::pow(10.0,-9.0/20),"preview samples");
    const auto applied=runtime.applied_revision();
    request.revision=2;
    request.profile=serialize_profile({{{{"High",{},{{FilterType::peak,23000,2,1}}}}},48000,2});
    processor.submit(encode_request(request));
    require(processor.poll().state==PreviewState::rejected && processor.poll().revision==2 && !processor.poll().detail.empty(),
            "actual stream Nyquist rejection was not correlated");
    request.revision=3; request.profile=serialize_profile({gain(-2),48000,4}); processor.submit(encode_request(request));
    require(processor.poll().state==PreviewState::rejected,"channel mismatch accepted");
    request.revision=4; request.profile="broken profile"; processor.submit(encode_request(request));
    require(processor.poll().state==PreviewState::rejected && processor.poll().revision==4,"malformed profile lost request identity");
    samples.fill(0.1F); runtime.process(samples.data(),samples.data(),1000);
    require(runtime.applied_revision()==applied,"rejected request changed audio bank");
    test::expect_near(samples.back(),0.1*std::pow(10.0,-9.0/20),"last good audio preserved");
    request.revision=5; request.runtime=id(); request.profile=serialize_profile({gain(0),48000,2});
    processor.submit(encode_request(request));
    require(processor.poll().revision==4,"old runtime request was accepted");
    request.runtime=epoch; request.revision=1; processor.submit(encode_request(request));
    require(processor.poll().revision==4,"out-of-order request was accepted");
    processor.submit("malformed envelope"); processor.submit(encode_request(request));
    require(processor.poll().revision==0 && processor.poll().state==PreviewState::rejected,"malformed packet reset replay protection");
    processor.stop(); request.revision=10; processor.submit(encode_request(request));
    require(processor.poll().state==PreviewState::stopped,"stopped runtime accepted a new preview");
}
void bridge() {
    const auto store=Microsoft::WRL::Make<control_test::Store>();
    EditSession editor({gain(-2),48000,2});
    LivePreview client(editor,store.Get());
    rejects([&] { client.connect(); });
    SmoothEq runtime; runtime.initialize(ResidentBank::prepare(gain(-2),48000,2,1),960);
    PreviewProcessor processor(runtime,48000,2);
    const auto report=[&] { write_control_value(*store.Get(),preview_status_key,encode_status(processor.poll())); };
    report(); client.connect();
    editor.replace(gain(-4)); client.submit();
    const auto skipped=editor.snapshot();
    editor.replace(gain(-8)); client.submit();
    require(client.poll().feedback==PreviewFeedback::waiting && !editor.applied(),"property write claimed audio applied");
    const auto request=read_control_value(*store.Get(),preview_request_key,max_request_bytes);
    require(request.has_value(),"preview property missing");
    processor.submit(*request); report();
    require(client.poll().feedback==PreviewFeedback::waiting,"queue response claimed audio applied");
    std::array<float,2000> buffer{}; buffer.fill(0.1F); runtime.process(buffer.data(),buffer.data(),1000);
    report(); require(client.poll().feedback==PreviewFeedback::applied && editor.applied()==editor.snapshot(),"client did not correlate actual application");
    require(editor.applied()!=skipped && editor.dirty(),"coalesced edit or preview changed the saved checkpoint");
    processor.submit(encode_request({processor.poll().runtime,id(),1,serialize_profile({gain(-12),48000,2})})); report();
    require(client.poll().feedback==PreviewFeedback::disconnected && !editor.applied(),"another editor left a false active claim");
    client.connect(); client.submit();
    processor.submit(*read_control_value(*store.Get(),preview_request_key,max_request_bytes));
    runtime.process(buffer.data(),buffer.data(),1000); report();
    require(client.poll().feedback==PreviewFeedback::applied,"explicit reconnection after another editor failed");
    auto stale=processor.poll(); stale.heartbeat_ms=GetTickCount64()-control_timeout_ms-1;
    write_control_value(*store.Get(),preview_status_key,encode_status(stale));
    require(client.poll().feedback==PreviewFeedback::disconnected && !editor.applied(),"stale engine retained active claim");
    PreviewProcessor restarted(runtime,48000,2);
    write_control_value(*store.Get(),preview_status_key,encode_status(restarted.poll()));
    rejects([&] { client.submit(); });
    restarted.submit(*request);
    require(restarted.poll().state==PreviewState::ready,"restart replayed an old preview");
    client.connect(); client.submit();
    restarted.submit(*read_control_value(*store.Get(),preview_request_key,max_request_bytes));
    runtime.process(buffer.data(),buffer.data(),1000);
    write_control_value(*store.Get(),preview_status_key,encode_status(restarted.poll()));
    require(client.poll().feedback==PreviewFeedback::applied,"explicit reconnection did not work");
    write_control_value(*store.Get(),preview_status_key,encode_status(restarted.stop()));
    require(client.poll().feedback==PreviewFeedback::disconnected && !editor.applied(),"stopped engine retained active claim");
    PROPVARIANT invalid{}; invalid.vt=VT_UI4; invalid.ulVal=7;
    require(SUCCEEDED(store->SetValue(preview_status_key,invalid)),"write malformed fixture");
    require(client.poll().feedback==PreviewFeedback::disconnected,"wrong property type accepted");
}
void saved_settings() {
    const auto store=Microsoft::WRL::Make<control_test::Store>();
    const auto preview=Microsoft::WRL::Make<control_test::Store>();
    const auto endpoint=Microsoft::WRL::Make<control_test::Endpoint>(preview.Get(),store.Get());
    const auto devices=Microsoft::WRL::Make<control_test::Devices>(endpoint.Get());
    Microsoft::WRL::ComPtr<IPropertyStore> writer,reader;
    require(SUCCEEDED(open_saved_store(endpoint.Get(),STGM_READWRITE,&writer)) &&
            SUCCEEDED(open_saved_store(devices.Get(),STGM_READ,&reader)),"open persistent stores through endpoint/collection");
    require(!read_saved_profile(*reader.Get()),"missing settings were not distinct from malformed settings");
    EditSession editor({gain(-2),48000,2});
    editor.replace(gain(-7)); save_profile_settings(editor,*writer.Get());
    require(!editor.dirty() && editor.can_undo() && !editor.applied(),"save lost history or claimed audio application");
    require(read_saved_profile(*reader.Get())==std::optional{editor.saved()},"reopened reader lost the saved profile");
    require(!read_control_value(*preview.Get(),preview_request_key,max_request_bytes),"save published an unsolicited preview");
    const auto checkpoint=editor.saved();
    editor.replace(gain(-12));
    const auto fails=[&] {
        bool failed=false; try { save_profile_settings(editor,*writer.Get()); } catch (const std::exception&) { failed=true; }
        require(failed && editor.dirty() && editor.saved()==checkpoint && editor.can_undo() && !editor.applied(),
                "failed settings save advanced the checkpoint, lost history, or claimed application");
    };
    store->write_result=E_ACCESSDENIED; fails(); store->write_result=S_OK;
    require(read_saved_profile(*reader.Get())==std::optional{checkpoint},"denied SetValue changed persisted data");
    store->discard_writes=true; fails(); store->discard_writes=false;
    store->commit_result=E_FAIL; fails(); store->commit_result=S_OK;
    store->read_result=E_FAIL; fails(); store->read_result=S_OK;
    save_profile_settings(editor,*writer.Get());
    require(!editor.dirty() && read_saved_profile(*reader.Get())==std::optional{editor.saved()},"retry did not reconcile saved state");
    editor.replace(gain(-1)); require(editor.revert(),"revert did not use the settings checkpoint");
    require(editor.working()==editor.saved() && editor.working().configuration==gain(-12),"wrong settings checkpoint after revert");
    write_control_value(*store.Get(),saved_profile_key,"CONTOUR unsupported");
    rejects([&] { read_saved_profile(*reader.Get()); });
    PROPVARIANT bad{}; bad.vt=VT_UI4; bad.ulVal=1;
    require(SUCCEEDED(store->SetValue(saved_profile_key,bad)),"malformed saved fixture");
    rejects([&] { read_saved_profile(*reader.Get()); });
    write_control_value(*store.Get(),saved_profile_key,std::string(max_saved_profile_bytes+1,'x'));
    rejects([&] { read_saved_profile(*reader.Get()); });
}
void processing_evidence() {
    const auto initial=processing_fingerprint(gain(-3),48000,2);
    auto renamed=gain(-3); renamed.layers[0].name="Different display text";
    require(processing_fingerprint(renamed,48000,2)==initial,"Display metadata changed processing identity");
    require(processing_fingerprint(gain(-6),48000,2)!=initial && processing_fingerprint(gain(-3),44100,2)!=initial,"Gain/format changes reused processing identity");
    renamed.bypass=true; require(processing_fingerprint(renamed,48000,2)==processing_fingerprint({},48000,2),"Bypass was not identified as neutral processing");
    SmoothEq runtime; runtime.initialize(ResidentBank::prepare(gain(-3),48000,2,0),960);
    PreviewProcessor processor(runtime,48000,2,{},initial);
    require(processor.poll().evidence==RuntimeEvidence::loaded && processor.poll().active_fingerprint==initial,"Loaded bank claimed processed samples");
    runtime.process(nullptr,nullptr,0); require(processor.poll().evidence==RuntimeEvidence::loaded,"Zero frames counted as processing");
    std::array<float,4000> samples{}; samples.fill(0.1F); runtime.process(samples.data(),samples.data(),2000);
    require(processor.poll().evidence==RuntimeEvidence::processed && processor.poll().active_fingerprint==initial,"Initial audio callback did not confirm loaded bank");
    const auto epoch=processor.poll().runtime,session=id();
    const auto submit=[&](std::uint64_t revision,double db) { processor.submit(encode_request({epoch,session,revision,serialize_profile({gain(db),48000,2})})); };
    submit(1,-6); require(processor.poll().evidence==RuntimeEvidence::transitioning,"Queued update reported a settled runtime");
    runtime.process(samples.data(),samples.data(),100); submit(2,-9); submit(3,-12);
    processor.submit(encode_request({epoch,session,4,"invalid profile"}));
    require(processor.poll().state==PreviewState::rejected && processor.poll().evidence==RuntimeEvidence::transitioning,"Rejected later request lost the accepted transition");
    runtime.process(samples.data(),samples.data(),2000);
    require(processor.poll().evidence==RuntimeEvidence::transitioning,"Coalesced pending bank was mistaken for completed processing");
    samples.fill(0.1F); runtime.process(samples.data(),samples.data(),2000);
    require(processor.poll().evidence==RuntimeEvidence::processed && processor.poll().active_fingerprint==processing_fingerprint(gain(-12),48000,2),"Actual bank identity was lost after coalescing/rejection");
    test::expect_near(samples.back(),0.1*std::pow(10.0,-12.0/20),"Fingerprint identifies actual DSP output");
    processor.stop(); require(processor.poll().evidence==RuntimeEvidence::unknown && !processor.poll().active_fingerprint,"Stopped runtime retained processing evidence");
}
} // namespace
int main() {
    const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if (FAILED(com)) { return 2; }
    test::Suite suite;
    suite.run("bounded protocol, complete envelopes, versioning, and identity validation",roundtrips);
    suite.run("actual-rate/channel validation, audio acknowledgement, rejection, and stale requests",server_validation);
    suite.run("property-store preview, coalescing, stale status, restart, reconnect, and stop",bridge);
    suite.run("persistent settings, save/revert checkpoint, failed writes/commit/readback, and invalid data",saved_settings);
    suite.run("compiled processing identity, initial-frame evidence, crossfades, coalescing, rejection, stop",processing_evidence);
    const auto result=suite.finish(); CoUninitialize(); return result;
}
