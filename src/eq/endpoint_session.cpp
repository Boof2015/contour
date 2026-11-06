#include <initguid.h>
#include <contour/endpoint_session.h>
#include <wrl.h>
#include <atomic>
#include <stdexcept>
#include <utility>

namespace contour::eq {
namespace {
using Microsoft::WRL::ComPtr;
void hr(HRESULT value, const char* operation) {
    if (FAILED(value)) { throw std::runtime_error(std::string(operation)+"; HRESULT "+std::to_string(value)); }
}
void require(bool condition, const char* message) { if (!condition) { throw std::runtime_error(message); } }

// No owner pointer, locks, allocation, COM calls, or UI work in callbacks.
// The observer itself stays alive until notification unregistration completes.
class Notifications final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMMNotificationClient, Microsoft::WRL::FtmBase> {
public:
    explicit Notifications(std::wstring id) : id_(std::move(id)) {}
    std::atomic<std::uint64_t> generation{0};
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id,DWORD) override { changed(id); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR id) override { changed(id); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override { changed(id); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow,ERole,LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR id,const PROPERTYKEY key) override {
        if (IsEqualPropertyKey(key,PKEY_AudioEngine_DeviceFormat) || IsEqualPropertyKey(key,PKEY_AudioEndpoint_PhysicalSpeakers)) { changed(id); }
        return S_OK;
    }
private:
    void changed(LPCWSTR id) noexcept { if (id && id_==id) { generation.fetch_add(1,std::memory_order_release); } }
    const std::wstring id_;
};
}

struct EndpointSession::Impl {
    EditSession* editor;
    const std::wstring id;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<Notifications> notifications;
    ComPtr<IPropertyStore> store;
    std::unique_ptr<LivePreview> client;
    EndpointStatus status;
    std::uint64_t generation=0;
    GUID bound_runtime{}, retired_runtime{};
    EditSession::Snapshot validated;
    double validated_rate=0;
    std::uint32_t validated_channels=0;
    std::string validation_error;
    std::optional<bool> bypass_override;
    Profile processing(Profile value) const {
        if (bypass_override) { value.configuration.bypass=*bypass_override; }
        return value;
    }

    Impl(EditSession& value,std::wstring endpoint,IMMDeviceEnumerator* supplied)
        : editor(&value),id(std::move(endpoint)),enumerator(supplied) {
        require(!id.empty(),"An explicit endpoint ID is required");
        if (!enumerator) { hr(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)),"Create endpoint enumerator"); }
        notifications=Microsoft::WRL::Make<Notifications>(id);
        if (!notifications) { throw std::bad_alloc(); }
        hr(enumerator->RegisterEndpointNotificationCallback(notifications.Get()),"Register endpoint notifications");
        editor->clear_applied();
    }
    ~Impl() {
        drop();
        // If the service fails to unregister, keep the callback allocation
        // alive rather than risk a late call into freed memory. It owns no app
        // objects, so a late callback cannot touch the destroyed editor.
        if (FAILED(enumerator->UnregisterEndpointNotificationCallback(notifications.Get()))) { (void)notifications.Detach(); }
    }
    void drop() noexcept { client.reset(); store.Reset(); bound_runtime=GUID_NULL; editor->clear_applied(); }
    void invalidate() {
        if (status.runtime) { retired_runtime=status.runtime->runtime; }
        drop(); validated.reset();
        status={EndpointState::waiting_for_audio,0,{},PreviewFeedback::disconnected,"Device changed; waiting for its audio runtime"};
    }
    std::optional<PreviewStatus> read_runtime() {
        const auto bytes=read_control_value(*store.Get(),preview_status_key,max_status_bytes);
        return bytes ? std::optional{decode_status(*bytes)} : std::nullopt;
    }
    bool compatible(const PreviewStatus& runtime) {
        if (validated!=editor->snapshot() || validated_rate!=runtime.sample_rate || validated_channels!=runtime.channels) {
            validated=editor->snapshot(); validated_rate=runtime.sample_rate; validated_channels=runtime.channels;
            validation_error.clear();
            try {
                require(editor->working().channels==runtime.channels,"Profile channel count does not match this device; explicit mapping is required");
                ParametricEq check; check.configure(processing(editor->working()).configuration,runtime.sample_rate,runtime.channels);
            } catch (const std::exception& e) { validation_error=e.what(); }
        }
        return validation_error.empty();
    }
    void observe() {
        ComPtr<IMMDevice> endpoint;
        const auto found=enumerator->GetDevice(id.c_str(),&endpoint);
        if (found==E_NOTFOUND) {
            if (status.runtime) { retired_runtime=status.runtime->runtime; }
            drop(); status={EndpointState::unavailable,0,{},PreviewFeedback::disconnected,"Selected device is not present"}; return;
        }
        hr(found,"Find selected endpoint");
        DWORD state=0; hr(endpoint->GetState(&state),"Read selected endpoint state");
        if (state!=DEVICE_STATE_ACTIVE) {
            if (status.runtime) { retired_runtime=status.runtime->runtime; }
            drop(); status={EndpointState::unavailable,state,{},PreviewFeedback::disconnected,"Selected device is not active"}; return;
        }
        status.device_state=state;
        if (!store) {
            const auto opened=open_control_store(endpoint.Get(),&store);
            if (opened==E_NOTFOUND) {
                drop(); status={EndpointState::not_attached,state,{},PreviewFeedback::disconnected,"Contour settings are not installed on this device"}; return;
            }
            hr(opened,"Open selected endpoint settings");
        }
        const auto runtime=read_runtime();
        if (!runtime || !status_is_live(*runtime,GetTickCount64()) || runtime->runtime==retired_runtime) {
            drop(); status={EndpointState::waiting_for_audio,state,{},PreviewFeedback::disconnected,"Waiting for a responsive Contour audio runtime"}; return;
        }
        status.runtime=runtime;
        if (bound_runtime!=GUID_NULL && bound_runtime!=runtime->runtime) { client.reset(); bound_runtime=GUID_NULL; editor->clear_applied(); }
        if (!compatible(*runtime)) {
            client.reset(); bound_runtime=GUID_NULL; editor->clear_applied();
            status.state=EndpointState::incompatible; status.preview=PreviewFeedback::disconnected; status.detail=validation_error; return;
        }
        if (!client) {
            auto next=std::make_unique<LivePreview>(*editor,store.Get(),bypass_override);
            const auto connected=next->connect();
            require(connected.runtime==runtime->runtime && connected.sample_rate==runtime->sample_rate && connected.channels==runtime->channels,
                    "Audio runtime changed while reconnecting; refresh again");
            client=std::move(next); bound_runtime=connected.runtime;
        }
        const auto reply=client->poll();
        if (reply.feedback==PreviewFeedback::disconnected) {
            drop(); status={EndpointState::waiting_for_audio,state,{},reply.feedback,reply.detail}; return;
        }
        status.state=EndpointState::ready;
        status.preview=reply.feedback==PreviewFeedback::applied && editor->awaiting_preview() ? PreviewFeedback::ready : reply.feedback;
        status.detail=reply.detail;
    }
    const EndpointStatus& refresh() {
        const auto observed=notifications->generation.load(std::memory_order_acquire);
        if (observed!=generation) { invalidate(); generation=observed; }
        try { observe(); }
        catch (const std::exception& e) { drop(); status={EndpointState::error,0,{},PreviewFeedback::disconnected,e.what()}; }
        if (notifications->generation.load(std::memory_order_acquire)!=generation) { invalidate(); }
        return status;
    }
    void ensure_current() {
        require(notifications->generation.load(std::memory_order_acquire)==generation,"Device changed during the operation; refresh before retrying");
        const auto current=read_runtime();
        require(current && status_is_live(*current,GetTickCount64()) && status.runtime && current->runtime==status.runtime->runtime &&
                current->sample_rate==status.runtime->sample_rate && current->channels==status.runtime->channels,
                "Audio runtime changed during the operation; refresh before retrying");
    }
};

const char* endpoint_state_name(EndpointState state) noexcept {
    switch (state) {
    case EndpointState::unavailable: return "unavailable";
    case EndpointState::not_attached: return "not_attached";
    case EndpointState::waiting_for_audio: return "waiting_for_audio";
    case EndpointState::incompatible: return "incompatible";
    case EndpointState::ready: return "ready";
    case EndpointState::error: return "error";
    }
    return "unknown";
}
EndpointSession::EndpointSession(EditSession& editor,std::wstring id,IMMDeviceEnumerator* enumerator)
    : impl_(std::make_unique<Impl>(editor,std::move(id),enumerator)) {}
EndpointSession::~EndpointSession()=default;
const std::wstring& EndpointSession::endpoint_id() const noexcept { return impl_->id; }
std::uint64_t EndpointSession::device_change_count() const noexcept { return impl_->notifications->generation.load(std::memory_order_acquire); }
const EndpointStatus& EndpointSession::refresh() { return impl_->refresh(); }
void EndpointSession::rebind(EditSession& editor) noexcept {
    auto& s=*impl_;
    if (s.editor==&editor) { return; }
    s.drop(); s.editor=&editor; editor.clear_applied(); s.validated.reset(); s.validation_error.clear();
    s.status.state=EndpointState::waiting_for_audio; s.status.preview=PreviewFeedback::disconnected; s.status.detail.clear();
}
void EndpointSession::preview() {
    auto& s=*impl_;
    require(s.refresh().state==EndpointState::ready,"Selected device is not ready for this EQ");
    try {
        s.ensure_current(); s.client->submit(); s.ensure_current();
        s.status.preview=PreviewFeedback::waiting;
    } catch (const std::exception& e) { s.drop(); s.status.state=EndpointState::error; s.status.preview=PreviewFeedback::disconnected; s.status.detail=e.what(); throw; }
}
void EndpointSession::set_bypass_override(std::optional<bool> bypass) noexcept {
    auto& s=*impl_;
    if (s.bypass_override==bypass) { return; }
    s.bypass_override=bypass; s.drop(); s.validated.reset();
}
void EndpointSession::save() {
    impl_->editor->save_with([&](const Profile& profile) { persist(profile); });
}
EndpointCopyReceipt EndpointSession::persist(const Profile& profile) {
    auto& s=*impl_;
    const auto state=s.refresh().state;
    require(state==EndpointState::ready || state==EndpointState::incompatible,"Selected device is not ready to save this EQ");
    try {
        validate_profile(profile);
        require(s.status.runtime && profile.channels==s.status.runtime->channels,"Saved profile channel count does not match this device; explicit mapping is required");
        const auto processing=s.processing(profile);
        ParametricEq check; check.configure(processing.configuration,s.status.runtime->sample_rate,s.status.runtime->channels);
        ComPtr<IMMDevice> endpoint; hr(s.enumerator->GetDevice(s.id.c_str(),&endpoint),"Reopen selected endpoint");
        ComPtr<IPropertyStore> saved; hr(open_saved_store(endpoint.Get(),STGM_READWRITE,&saved),"Open selected endpoint saved settings");
        s.ensure_current(); write_saved_profile(*saved.Get(),processing); s.ensure_current();
        return {s.status.runtime->runtime,s.generation};
    } catch (const std::exception& e) { s.drop(); s.status.state=EndpointState::error; s.status.preview=PreviewFeedback::disconnected; s.status.detail=e.what(); throw; }
}
void EndpointSession::persist_inactive(const Profile& profile) {
    auto& s=*impl_; const auto generation=s.notifications->generation.load(std::memory_order_acquire);
    validate_profile(profile);
    ComPtr<IMMDevice> endpoint; hr(s.enumerator->GetDevice(s.id.c_str(),&endpoint),"Find device for saved EQ");
    ComPtr<IPropertyStore> saved; hr(open_saved_store(endpoint.Get(),STGM_READWRITE,&saved),"Open device saved settings");
    write_saved_profile(*saved.Get(),s.processing(profile));
    require(s.notifications->generation.load(std::memory_order_acquire)==generation,"Device changed while saving EQ");
}
EndpointSavedSnapshot EndpointSession::inspect_saved() {
    auto& s=*impl_; const auto generation=s.notifications->generation.load(std::memory_order_acquire);
    ComPtr<IMMDevice> endpoint; hr(s.enumerator->GetDevice(s.id.c_str(),&endpoint),"Find selected endpoint for inspection");
    DWORD state=0; hr(endpoint->GetState(&state),"Read inspected endpoint state");
    require(state==DEVICE_STATE_ACTIVE,"Selected device is not active");
    ComPtr<IPropertyStore> saved; hr(open_saved_store(endpoint.Get(),STGM_READ,&saved),"Open saved settings read-only");
    auto profile=read_saved_profile(*saved.Get());
    hr(endpoint->GetState(&state),"Recheck inspected endpoint state");
    require(state==DEVICE_STATE_ACTIVE && s.notifications->generation.load(std::memory_order_acquire)==generation,
            "Device changed during inspection; inspect again");
    return {std::move(profile),generation};
}
} // namespace contour::eq
