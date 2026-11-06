#pragma once
#include <contour/live_control.h>
#include <propvarutil.h>
#include <wrl.h>
#include <mutex>
#include <functional>
#include <stdexcept>

namespace control_test {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::FtmBase;

// Thread-safe, marshalable fixture backed by Windows' in-memory property store.
// This substitutes only the endpoint's storage, not the APO or processing code.
class Store final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IPropertyStore, FtmBase> {
public:
    // Configure faults only while no other thread uses this store.
    HRESULT read_result=S_OK, write_result=S_OK, commit_result=S_OK;
    bool discard_writes=false;
    unsigned writes=0,commits=0;
    std::function<void()> after_commit;
    Store() {
        if (FAILED(PSCreateMemoryPropertyStore(IID_PPV_ARGS(&values_)))) { throw std::runtime_error("Create fixture store"); }
    }
    HRESULT STDMETHODCALLTYPE GetCount(DWORD* count) override { std::lock_guard lock(mutex_); return values_->GetCount(count); }
    HRESULT STDMETHODCALLTYPE GetAt(DWORD index, PROPERTYKEY* key) override { std::lock_guard lock(mutex_); return values_->GetAt(index,key); }
    HRESULT STDMETHODCALLTYPE GetValue(REFPROPERTYKEY key, PROPVARIANT* property) override { std::lock_guard lock(mutex_); return FAILED(read_result) ? read_result : values_->GetValue(key,property); }
    HRESULT STDMETHODCALLTYPE SetValue(REFPROPERTYKEY key, REFPROPVARIANT property) override { std::lock_guard lock(mutex_); ++writes; return FAILED(write_result) || discard_writes ? write_result : values_->SetValue(key,property); }
    HRESULT STDMETHODCALLTYPE Commit() override {
        HRESULT result;
        { std::lock_guard lock(mutex_); ++commits; result=FAILED(commit_result) ? commit_result : values_->Commit(); }
        if (after_commit) { after_commit(); } return result;
    }
private:
    std::mutex mutex_;
    ComPtr<IPropertyStore> values_;
};

class Endpoint final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMMDevice, IAudioSystemEffectsPropertyStore> {
public:
    DWORD device_state=DEVICE_STATE_ACTIVE;
    HRESULT activation_result=S_OK;
    std::function<void()> before_activate;
    explicit Endpoint(IPropertyStore* store, IPropertyStore* saved=nullptr) : store_(store),saved_(saved) {}
    HRESULT STDMETHODCALLTYPE Activate(REFIID iid, DWORD, PROPVARIANT* context, void** result) override {
        if (!result) { return E_POINTER; } *result=nullptr;
        if (before_activate) { before_activate(); }
        if (FAILED(activation_result)) { return activation_result; }
        if (iid!=__uuidof(IAudioSystemEffectsPropertyStore) || !context || context->vt!=VT_CLSID ||
            !context->puuid || *context->puuid!=contour::eq::control_context) { return E_NOINTERFACE; }
        return QueryInterface(iid,result);
    }
    HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD, IPropertyStore**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetId(LPWSTR*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetState(DWORD* state) override { if (!state) { return E_POINTER; } *state=device_state; return S_OK; }
    HRESULT STDMETHODCALLTYPE OpenDefaultPropertyStore(DWORD, IPropertyStore**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OpenUserPropertyStore(DWORD access, IPropertyStore** store) override {
        if (!store) { return E_POINTER; } *store=nullptr;
        if (access!=STGM_READ && access!=STGM_READWRITE) { return E_ACCESSDENIED; }
        return saved_ ? saved_.CopyTo(store) : E_NOTFOUND;
    }
    HRESULT STDMETHODCALLTYPE OpenVolatilePropertyStore(DWORD access, IPropertyStore** store) override {
        return access==STGM_READWRITE ? store_.CopyTo(store) : E_ACCESSDENIED;
    }
    HRESULT STDMETHODCALLTYPE ResetUserPropertyStore() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ResetVolatilePropertyStore() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RegisterPropertyChangeNotification(IAudioSystemEffectsPropertyChangeNotificationClient*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnregisterPropertyChangeNotification(IAudioSystemEffectsPropertyChangeNotificationClient*) override { return E_NOTIMPL; }
private:
    ComPtr<IPropertyStore> store_;
    ComPtr<IPropertyStore> saved_;
};

// Mirrors MMDevice's callback ownership contract: registration borrows the
// callback. Only the app controller owns its lifetime until unregistration.
class Enumerator final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMMDeviceEnumerator> {
public:
    explicit Enumerator(std::wstring id,IMMDevice* endpoint) : id_(std::move(id)),endpoint_(endpoint) {}
    HRESULT lookup_result=S_OK;
    std::function<void()> before_lookup;
    IMMNotificationClient* callback=nullptr;
    unsigned registrations=0,unregistrations=0,default_queries=0,lookups=0;
    void endpoint(IMMDevice* device) { endpoint_=device; }
    HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow,DWORD,IMMDeviceCollection**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow,ERole,IMMDevice**) override { ++default_queries; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR id,IMMDevice** device) override {
        if (!device) { return E_POINTER; } *device=nullptr; ++lookups;
        if (before_lookup) { before_lookup(); }
        if (FAILED(lookup_result)) { return lookup_result; }
        return id && id_==id ? endpoint_.CopyTo(device) : E_NOTFOUND;
    }
    HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient* observer) override {
        if (!observer || callback) { return E_INVALIDARG; } callback=observer; ++registrations; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient* observer) override {
        if (callback!=observer) { return E_INVALIDARG; } callback=nullptr; ++unregistrations; return S_OK;
    }
private:
    const std::wstring id_;
    ComPtr<IMMDevice> endpoint_;
};

class Devices final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMMDeviceCollection> {
public:
    explicit Devices(IMMDevice* endpoint) : endpoint_(endpoint) {}
    HRESULT STDMETHODCALLTYPE GetCount(UINT* count) override { if (!count) { return E_POINTER; } *count=2; return S_OK; }
    HRESULT STDMETHODCALLTYPE Item(UINT index, IMMDevice** device) override {
        if (!device) { return E_POINTER; } *device=nullptr;
        // Only the final entry is the endpoint, matching the Windows contract.
        return index==1 ? endpoint_.CopyTo(device) : E_INVALIDARG;
    }
private:
    ComPtr<IMMDevice> endpoint_;
};
} // namespace control_test
