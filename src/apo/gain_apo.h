#pragma once

#include <windows.h>
#include <baseaudioprocessingobject.h>
#include <contour/channel_gain.h>
#include <contour/live_eq.h>
#include <filesystem>
#include <string>

namespace contour::apo {

// Counts live factories, APOs, and explicit IClassFactory server locks.
extern volatile LONG module_references;
extern HINSTANCE module_instance;

#pragma AVRT_VTABLES_BEGIN
class GainApo final : public CBaseAudioProcessingObject, public IAudioSystemEffects {
public:
    explicit GainApo(bool post_mix, IUnknown* outer);
    ~GainApo() override;

    // Windows provides resident storage for the object and inline DSP state.
    static void* operator new(std::size_t size) noexcept;
    static void operator delete(void* memory) noexcept;

    // The factory returns this private identity to an aggregating engine.
    IUnknown* inner_unknown() noexcept { return &inner_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE Initialize(UINT32 size, BYTE* data) override;
    HRESULT STDMETHODCALLTYPE LockForProcess(UINT32 input_count,
        APO_CONNECTION_DESCRIPTOR** inputs, UINT32 output_count,
        APO_CONNECTION_DESCRIPTOR** outputs) override;
    HRESULT STDMETHODCALLTYPE UnlockForProcess() override;
    HRESULT STDMETHODCALLTYPE Reset() override;
    HRESULT STDMETHODCALLTYPE IsInputFormatSupported(IAudioMediaType* opposite,
        IAudioMediaType* requested, IAudioMediaType** supported) override;
    HRESULT STDMETHODCALLTYPE IsOutputFormatSupported(IAudioMediaType* opposite,
        IAudioMediaType* requested, IAudioMediaType** supported) override;
    void STDMETHODCALLTYPE APOProcess(UINT32 input_count, APO_CONNECTION_PROPERTY** inputs,
        UINT32 output_count, APO_CONNECTION_PROPERTY** outputs) override;

protected:
    HRESULT ValidateAndCacheConnectionInfo(UINT32 input_count,
        APO_CONNECTION_DESCRIPTOR** inputs, UINT32 output_count,
        APO_CONNECTION_DESCRIPTOR** outputs) override;

private:
    class InnerUnknown final : public IUnknown {
    public:
        explicit InnerUnknown(GainApo& owner) : owner_(owner) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override;
        ULONG STDMETHODCALLTYPE AddRef() override;
        ULONG STDMETHODCALLTYPE Release() override;
    private:
        GainApo& owner_;
    };
    HRESULT inner_query_interface(REFIID iid, void** result);
    HRESULT initialize_core(UINT32 size, BYTE* data);
    void report_processing_summary(const char* phase) const;
    void prepare_runtime();
    void stop_worker() noexcept;
    void check_profile();
    static unsigned __stdcall worker_entry(void* context) noexcept;
    InnerUnknown inner_{*this};
    // Borrowed controlling identity. AddRef here would create an ownership cycle.
    IUnknown* const controlling_unknown_;
    volatile LONG references_ = 1;
    const float gain_;
    const bool post_mix_;
    bool system_effect_ = false;
    dsp::ChannelGain processor_;
    eq::SmoothEq live_eq_;
    double sample_rate_ = 0;
    std::size_t channels_ = 0;
    std::filesystem::path profile_path_;
    std::string seen_profile_, last_profile_error_;
    std::uint64_t profile_revision_ = 0;
    HANDLE worker_stop_ = nullptr, worker_ = nullptr;
    UINT32 max_input_frames_ = 0;
    UINT32 max_output_frames_ = 0;
    // Only fixed-size atomic updates in the callback. ETW publication happens
    // in non-real-time lifecycle methods, never in APOProcess.
    alignas(8) volatile LONG64 process_calls_ = 0;
    alignas(8) volatile LONG64 processed_frames_ = 0;
    volatile LONG last_ratio_bits_ = 0;
    volatile LONG ratio_observed_ = 0;
};
#pragma AVRT_VTABLES_END

} // namespace contour::apo
