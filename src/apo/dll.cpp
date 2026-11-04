#include "gain_apo.h"
#include "diagnostics.h"
#include <contour/apo_ids.h>

#include <new>

namespace contour::apo {
TRACELOGGING_DEFINE_PROVIDER(diagnostic_provider, "Contour.Apo",
    (0xa4e1577a, 0x45ed, 0x4e67, 0xb1, 0xc2, 0x68, 0x48, 0x12, 0x5b, 0xa8, 0xc9));
volatile LONG module_references = 0;
HINSTANCE module_instance = nullptr;

namespace {
class ClassFactory final : public IClassFactory {
public:
    explicit ClassFactory(bool post_mix) : post_mix_(post_mix) {
        InterlockedIncrement(&module_references);
    }
    ~ClassFactory() { InterlockedDecrement(&module_references); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (result == nullptr) {
            return E_POINTER;
        }
        *result = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IClassFactory)) {
            return E_NOINTERFACE;
        }
        *result = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&references_));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = static_cast<ULONG>(InterlockedDecrement(&references_));
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** result) override {
        if (result == nullptr) {
            return E_POINTER;
        }
        *result = nullptr;
        if (outer != nullptr && iid != __uuidof(IUnknown)) {
            return E_NOINTERFACE;
        }
        auto* instance = new GainApo(post_mix_, outer);
        if (instance == nullptr) {
            TraceLoggingWrite(diagnostic_provider, "CreateInstance",
                TraceLoggingBool(post_mix_, "PostMix"), TraceLoggingGuid(iid, "InterfaceId"),
                TraceLoggingHResult(E_OUTOFMEMORY, "Result"));
            return E_OUTOFMEMORY;
        }
        const HRESULT status = instance->inner_unknown()->QueryInterface(iid, result);
        TraceLoggingWrite(diagnostic_provider, "CreateInstance",
            TraceLoggingBool(post_mix_, "PostMix"), TraceLoggingGuid(iid, "InterfaceId"),
            TraceLoggingBool(outer != nullptr, "Aggregated"), TraceLoggingHResult(status, "Result"));
        instance->inner_unknown()->Release();
        return status;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock) {
            InterlockedIncrement(&module_references);
        } else {
            InterlockedDecrement(&module_references);
        }
        return S_OK;
    }

private:
    volatile LONG references_ = 1;
    const bool post_mix_;
};
} // namespace
} // namespace contour::apo

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        contour::apo::module_instance = instance;
        TraceLoggingRegister(contour::apo::diagnostic_provider);
        TraceLoggingWrite(contour::apo::diagnostic_provider, "ModuleAttach");
    } else if (reason == DLL_PROCESS_DETACH) {
        TraceLoggingUnregister(contour::apo::diagnostic_provider);
    }
    return TRUE;
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID iid, void** result) {
    if (result == nullptr) {
        return E_POINTER;
    }
    *result = nullptr;
    using namespace contour::apo;
    if (clsid != pre_mix_clsid && clsid != post_mix_clsid) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    auto* factory = new (std::nothrow) ClassFactory(clsid == post_mix_clsid);
    if (factory == nullptr) {
        return E_OUTOFMEMORY;
    }
    const HRESULT status = factory->QueryInterface(iid, result);
    TraceLoggingWrite(diagnostic_provider, "GetClassObject",
        TraceLoggingGuid(clsid, "ClassId"), TraceLoggingGuid(iid, "InterfaceId"),
        TraceLoggingHResult(status, "Result"));
    factory->Release();
    return status;
}

extern "C" HRESULT __stdcall DllCanUnloadNow() {
    return InterlockedCompareExchange(&contour::apo::module_references, 0, 0) == 0 ? S_OK : S_FALSE;
}
