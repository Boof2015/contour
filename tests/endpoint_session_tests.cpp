#include <contour/endpoint_session.h>
#include "control_test_support.h"
#include "test_support.h"
#include <array>
#include <thread>

using namespace contour::eq;
using test::require;
namespace {
constexpr auto endpoint_id=L"opaque-selected-endpoint";
Configuration gain(double value) { return {{{"All",{},{},value}}}; }
template<class F> void fails(F action) {
    bool failed=false; try { action(); } catch (const std::exception&) { failed=true; }
    require(failed,"Expected rejected operation");
}
struct Fixture {
    Microsoft::WRL::ComPtr<control_test::Store> volatile_store=Microsoft::WRL::Make<control_test::Store>();
    Microsoft::WRL::ComPtr<control_test::Store> saved=Microsoft::WRL::Make<control_test::Store>();
    Microsoft::WRL::ComPtr<control_test::Endpoint> device=Microsoft::WRL::Make<control_test::Endpoint>(volatile_store.Get(),saved.Get());
    Microsoft::WRL::ComPtr<control_test::Enumerator> enumerator=Microsoft::WRL::Make<control_test::Enumerator>(endpoint_id,device.Get());
    EditSession editor{{gain(-2),48000,2}};
    SmoothEq runtime;
    std::unique_ptr<PreviewProcessor> processor;
    EndpointSession session{editor,endpoint_id,enumerator.Get()};
    void restart(double rate=48000,std::size_t channels=2) {
        processor.reset(); runtime.initialize(ResidentBank::prepare(gain(-2),rate,channels,1),static_cast<std::uint32_t>(rate*0.02));
        processor=std::make_unique<PreviewProcessor>(runtime,rate,channels); report();
    }
    void report() { write_control_value(*volatile_store.Get(),preview_status_key,encode_status(processor->poll())); }
    void apply() {
        const auto request=read_control_value(*volatile_store.Get(),preview_request_key,max_request_bytes);
        require(request.has_value(),"Missing requested preview"); processor->submit(*request); report();
        require(session.refresh().preview==PreviewFeedback::waiting,"Queued update claimed application");
        std::array<float,4000> samples{}; runtime.process(samples.data(),samples.data(),2000); report();
        require(session.refresh().preview==PreviewFeedback::applied && editor.applied()==editor.snapshot(),"Completed update not acknowledged");
    }
    void replace_endpoint() {
        volatile_store=Microsoft::WRL::Make<control_test::Store>();
        device=Microsoft::WRL::Make<control_test::Endpoint>(volatile_store.Get(),saved.Get());
        enumerator->endpoint(device.Get());
    }
};
void unplug_replug() {
    Fixture f; f.restart();
    require(f.session.refresh().state==EndpointState::ready,"Initial binding failed");
    f.editor.replace(gain(-7)); f.session.preview(); f.apply();
    const auto draft=f.editor.snapshot(); const auto checkpoint=f.editor.saved();
    const auto old_store=f.volatile_store;
    const auto old_request=read_control_value(*old_store.Get(),preview_request_key,max_request_bytes);
    std::thread notify([&] { f.enumerator->callback->OnDeviceRemoved(endpoint_id); }); notify.join();
    f.enumerator->lookup_result=E_NOTFOUND;
    require(f.session.refresh().state==EndpointState::unavailable && !f.editor.applied(),"Removal retained audio confirmation");
    fails([&] { f.session.preview(); }); fails([&] { f.session.save(); });
    require(f.editor.snapshot()==draft && f.editor.saved()==checkpoint && f.editor.can_undo(),"Unplug lost draft/checkpoint/history");
    // A quick replug with the old fresh heartbeat must not revive the old graph.
    f.enumerator->lookup_result=S_OK; f.enumerator->callback->OnDeviceAdded(endpoint_id);
    require(f.session.refresh().state==EndpointState::waiting_for_audio,"Replug accepted the retired runtime");
    f.replace_endpoint(); f.restart(44100);
    require(f.session.refresh().state==EndpointState::ready && !f.editor.applied(),"New store/runtime was not rebound");
    require(!read_control_value(*f.volatile_store.Get(),preview_request_key,max_request_bytes),"Reconnect automatically replayed the draft");
    require(read_control_value(*old_store.Get(),preview_request_key,max_request_bytes)==old_request,"Reconnect wrote the old store");
    require(f.editor.snapshot()==draft && f.editor.undo() && f.editor.redo() && f.editor.working()==draft->profile(),"Reconnect changed edit history");
    f.session.preview(); f.apply();
    require(f.enumerator->default_queries==0 && f.session.endpoint_id()==endpoint_id,"Selection followed another device");
}
void format_changes() {
    Fixture f; f.restart(); f.session.refresh();
    auto config=gain(-4); config.layers[0].filters.push_back({FilterType::peak,23000,-2,1});
    f.editor.replace(config); f.session.preview(); f.apply(); const auto draft=f.editor.snapshot();
    f.enumerator->callback->OnPropertyValueChanged(endpoint_id,PKEY_AudioEngine_DeviceFormat);
    require(f.session.refresh().state==EndpointState::waiting_for_audio && !f.editor.applied(),"Format notification retained old graph");
    f.restart(44100);
    require(f.session.refresh().state==EndpointState::incompatible && !f.session.refresh().detail.empty(),"Lower Nyquist was not checked before preview");
    fails([&] { f.session.preview(); }); fails([&] { f.session.save(); });
    require(f.editor.snapshot()==draft && f.editor.dirty(),"Incompatible rate rewrote the draft");
    f.editor.replace(gain(-4)); require(f.session.refresh().state==EndpointState::ready,"Valid edit did not recover readiness");
    f.session.preview(); f.apply();
    f.restart(48000,4); require(f.session.refresh().state==EndpointState::incompatible && !f.editor.applied(),"Channel-count change was silently remapped");
    fails([&] { f.session.save(); });
    f.restart(96000,2); require(f.session.refresh().state==EndpointState::ready && !f.editor.applied(),"Compatible runtime did not recover");
    f.session.preview(); f.apply(); f.session.save();
    require(!f.editor.dirty() && read_saved_profile(*f.saved.Get())==std::optional{f.editor.saved()},"Save after format recovery failed");
}
void failures_and_races() {
    Fixture f;
    f.device->activation_result=E_NOTFOUND;
    require(f.session.refresh().state==EndpointState::not_attached,"Missing installation reported as runtime-ready");
    f.device->activation_result=S_OK;
    require(f.session.refresh().state==EndpointState::waiting_for_audio,"Missing heartbeat reported as applied");
    f.restart(); f.session.refresh(); f.editor.replace(gain(-8)); f.session.preview(); f.apply();
    auto stale=f.processor->poll(); stale.heartbeat_ms=GetTickCount64()-control_timeout_ms-1;
    write_control_value(*f.volatile_store.Get(),preview_status_key,encode_status(stale));
    require(f.session.refresh().state==EndpointState::waiting_for_audio && !f.editor.applied(),"Stale heartbeat retained audio confirmation");
    f.report(); require(f.session.refresh().state==EndpointState::ready && !f.editor.applied(),"Heartbeat recovery replayed an edit");
    write_control_value(*f.volatile_store.Get(),preview_status_key,"broken status");
    require(f.session.refresh().state==EndpointState::error,"Malformed status was accepted");
    f.report(); f.session.refresh();
    const auto checkpoint=f.editor.saved(); f.saved->commit_result=E_FAIL;
    fails([&] { f.session.save(); }); f.saved->commit_result=S_OK;
    require(f.editor.saved()==checkpoint && f.editor.dirty(),"Failed Save lost dirty state");
    // The device changes while opening User settings, after preflight readiness.
    f.session.refresh();
    f.device->before_activate=[&] { f.enumerator->callback->OnDeviceRemoved(endpoint_id); };
    fails([&] { f.session.save(); });
    require(f.editor.saved()==checkpoint && f.editor.dirty() && !f.editor.applied(),"Concurrent removal falsely confirmed Save");
    f.device->before_activate={}; f.restart(); f.session.refresh(); f.session.save();
    require(!f.editor.dirty(),"Save did not recover after a new runtime");
}
void notifications_and_lifetime() {
    auto enumerator=Microsoft::WRL::Make<control_test::Enumerator>(endpoint_id,nullptr);
    {
        Fixture f; enumerator=f.enumerator; f.restart(); f.session.refresh();
        f.editor.replace(gain(-5)); f.session.preview(); f.apply();
        f.enumerator->callback->OnDefaultDeviceChanged(eRender,eConsole,L"other-device");
        f.enumerator->callback->OnDeviceRemoved(L"other-device");
        f.enumerator->callback->OnPropertyValueChanged(endpoint_id,saved_profile_key);
        require(f.session.refresh().preview==PreviewFeedback::applied && f.editor.applied(),"Unrelated notification invalidated selected audio");
        f.editor.replace(gain(-10));
        require(f.session.refresh().preview==PreviewFeedback::ready && f.editor.awaiting_preview(),"An unsubmitted draft was reported as applied");
        f.device->device_state=DEVICE_STATE_DISABLED;
        require(f.session.refresh().state==EndpointState::unavailable && !f.editor.applied(),"Missed notification left disabled endpoint ready");
        f.device->device_state=DEVICE_STATE_ACTIVE;
        require(f.session.refresh().state==EndpointState::waiting_for_audio,"Disabled graph was reused without a notification");
        f.restart(); require(f.session.refresh().state==EndpointState::ready,"State polling did not recover");
    }
    require(enumerator->callback==nullptr && enumerator->registrations==1 && enumerator->unregistrations==1,
            "Notification lifetime outlived the app controller");
}
}
int main() {
    const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED); if (FAILED(com)) { return 2; }
    test::Suite suite;
    suite.run("unplug/replug, exact endpoint selection, fresh stores, retained draft/history, explicit preview",unplug_replug);
    suite.run("runtime rate/channel changes, actual-Nyquist validation, blocked writes, and recovery",format_changes);
    suite.run("missing/stale/invalid runtime, failed Save, and notifications during operations",failures_and_races);
    suite.run("unrelated events, default-device changes, state polling, and callback lifetime",notifications_and_lifetime);
    const auto result=suite.finish(); CoUninitialize(); return result;
}
