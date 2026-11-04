#include "gain_apo.h"
#include "diagnostics.h"
#include <contour/apo_ids.h>
#include <contour/profile.h>
#include <process.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace contour::apo {
namespace {
constexpr auto flags = static_cast<APO_FLAG>(APO_FLAG_DEFAULT | APO_FLAG_INPLACE);
const APO_REG_PROPERTIES pre_properties = {pre_mix_clsid, flags,
    L"Contour pre-mix pass-through", L"Contour contributors", 0, 1,
    1, 1, 1, 1, ULONG_MAX, 1, {__uuidof(IAudioSystemEffects)}};
const APO_REG_PROPERTIES post_properties = {post_mix_clsid, flags,
    L"Contour post-mix EQ", L"Contour contributors", 0, 2,
    1, 1, 1, 1, ULONG_MAX, 1, {__uuidof(IAudioSystemEffects)}};
constexpr GUID float_format = {STATIC_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT};
// pow(10, -6 / 20), rounded once to float. No dB conversion in APOProcess.
constexpr float minus_six_db = 0.5011872336272722F;

bool supported_format(const UNCOMPRESSEDAUDIOFORMAT& format) noexcept {
    return IsEqualGUID(format.guidFormatType, float_format) &&
        format.dwBytesPerSampleContainer == sizeof(float) &&
        format.dwValidBitsPerSample == 32 &&
        format.dwSamplesPerFrame >= 1 && format.dwSamplesPerFrame <= dsp::max_channels &&
        std::isfinite(format.fFramesPerSecond) &&
        format.fFramesPerSecond >= 8000.0F &&
        format.fFramesPerSecond <= 384000.0F;
}

bool matching_formats(const UNCOMPRESSEDAUDIOFORMAT& a,
                      const UNCOMPRESSEDAUDIOFORMAT& b) noexcept {
    return a.dwSamplesPerFrame == b.dwSamplesPerFrame &&
        a.fFramesPerSecond == b.fFramesPerSecond && a.dwChannelMask == b.dwChannelMask;
}

HRESULT negotiate(IAudioMediaType* opposite, IAudioMediaType* requested,
                  IAudioMediaType** supported) {
    if (requested == nullptr) {
        return E_POINTER;
    }
    UNCOMPRESSEDAUDIOFORMAT request{};
    HRESULT result = requested->GetUncompressedAudioFormat(&request);
    if (FAILED(result)) {
        return APOERR_FORMAT_NOT_SUPPORTED;
    }
    UNCOMPRESSEDAUDIOFORMAT other{};
    if (opposite != nullptr) {
        result = opposite->GetUncompressedAudioFormat(&other);
        if (FAILED(result) || !supported_format(other)) {
            return APOERR_FORMAT_NOT_SUPPORTED;
        }
    }
    if (supported_format(request) &&
        (opposite == nullptr || matching_formats(request, other))) {
        if (supported != nullptr) {
            requested->AddRef();
            *supported = requested;
        }
        return S_OK;
    }
    if (opposite != nullptr) {
        if (supported != nullptr) {
            opposite->AddRef();
            *supported = opposite;
        }
        return S_FALSE;
    }
    // Offer float32 with the requested layout/rate when conversion is possible.
    request.guidFormatType = float_format;
    request.dwBytesPerSampleContainer = sizeof(float);
    request.dwValidBitsPerSample = 32;
    if (!supported_format(request)) {
        return APOERR_FORMAT_NOT_SUPPORTED;
    }
    if (supported != nullptr) {
        result = CreateAudioMediaTypeFromUncompressedAudioFormat(&request, supported);
        if (FAILED(result)) {
            return result;
        }
    }
    return S_FALSE;
}
} // namespace

GainApo::GainApo(bool post_mix, IUnknown* outer)
    : CBaseAudioProcessingObject(post_mix ? &post_properties : &pre_properties),
      controlling_unknown_(outer != nullptr ? outer : &inner_),
      gain_(post_mix ? minus_six_db : 1.0F), post_mix_(post_mix) {
    InterlockedIncrement(&module_references);
}

GainApo::~GainApo() {
    stop_worker();
    report_processing_summary("Destroy");
    InterlockedDecrement(&module_references);
}

void* GainApo::operator new(std::size_t size) noexcept {
    void* memory = nullptr;
    return SUCCEEDED(AERT_Allocate(size, &memory)) ? memory : nullptr;
}

void GainApo::operator delete(void* memory) noexcept {
    if (memory != nullptr) {
        AERT_Free(memory);
    }
}

HRESULT GainApo::QueryInterface(REFIID iid, void** result) {
    return controlling_unknown_->QueryInterface(iid, result);
}

ULONG GainApo::AddRef() { return controlling_unknown_->AddRef(); }
ULONG GainApo::Release() { return controlling_unknown_->Release(); }

HRESULT GainApo::InnerUnknown::QueryInterface(REFIID iid, void** result) {
    return owner_.inner_query_interface(iid, result);
}

HRESULT GainApo::inner_query_interface(REFIID iid, void** result) {
    if (result == nullptr) {
        return E_POINTER;
    }
    *result = nullptr;
    if (iid == __uuidof(IUnknown)) {
        *result = static_cast<IUnknown*>(&inner_);
        inner_.AddRef();
        return S_OK;
    } else if (iid == __uuidof(IAudioProcessingObject)) {
        *result = static_cast<IAudioProcessingObject*>(this);
    } else if (iid == __uuidof(IAudioProcessingObjectRT)) {
        *result = static_cast<IAudioProcessingObjectRT*>(this);
    } else if (iid == __uuidof(IAudioProcessingObjectConfiguration)) {
        *result = static_cast<IAudioProcessingObjectConfiguration*>(this);
    } else if (iid == __uuidof(IAudioSystemEffects)) {
        *result = static_cast<IAudioSystemEffects*>(this);
    } else {
        TraceLoggingWrite(diagnostic_provider, "UnsupportedInterface",
            TraceLoggingGuid(m_pRegProperties->clsid, "ClassId"), TraceLoggingGuid(iid, "InterfaceId"));
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

ULONG GainApo::InnerUnknown::AddRef() {
    return static_cast<ULONG>(InterlockedIncrement(&owner_.references_));
}

ULONG GainApo::InnerUnknown::Release() {
    const auto remaining = static_cast<ULONG>(InterlockedDecrement(&owner_.references_));
    if (remaining == 0) {
        delete &owner_;
    }
    return remaining;
}

HRESULT GainApo::Initialize(UINT32 size, BYTE* data) {
    APOInitBaseStruct header{};
    if (data != nullptr && size >= sizeof(header)) { std::memcpy(&header, data, sizeof(header)); }
    const HRESULT result = initialize_core(size, data);
    TraceLoggingWrite(diagnostic_provider, "Initialize",
        TraceLoggingGuid(m_pRegProperties->clsid, "ClassId"), TraceLoggingPointer(this, "Instance"),
        TraceLoggingUInt32(size, "DataSize"), TraceLoggingUInt32(header.cbSize, "HeaderSize"),
        TraceLoggingGuid(header.clsid, "HeaderClassId"), TraceLoggingHResult(result, "Result"));
    return result;
}

HRESULT GainApo::initialize_core(UINT32 size, BYTE* data) {
    if (m_bIsInitialized) {
        return APOERR_ALREADY_INITIALIZED;
    }
    if (size == 0 && data == nullptr) {
        return CBaseAudioProcessingObject::Initialize(0, nullptr);
    }
    // This prototype implements the legacy LFX/GFX initialization contract.
    // The single-endpoint lab package owns its sibling profile file.
    if (data == nullptr || size != sizeof(APOInitSystemEffects)) {
        return E_INVALIDARG;
    }
    APOInitBaseStruct header{};
    std::memcpy(&header, data, sizeof(header));
    if (header.cbSize != size) {
        return E_INVALIDARG;
    }
    if (header.clsid != m_pRegProperties->clsid) {
        return APOERR_INVALID_APO_CLSID;
    }
    const auto result = CBaseAudioProcessingObject::Initialize(0, nullptr);
    if (SUCCEEDED(result)) { system_effect_ = true; }
    return result;
}

HRESULT GainApo::IsInputFormatSupported(IAudioMediaType* opposite,
    IAudioMediaType* requested, IAudioMediaType** supported) {
    return negotiate(opposite, requested, supported);
}

HRESULT GainApo::IsOutputFormatSupported(IAudioMediaType* opposite,
    IAudioMediaType* requested, IAudioMediaType** supported) {
    return negotiate(opposite, requested, supported);
}

HRESULT GainApo::LockForProcess(UINT32 input_count, APO_CONNECTION_DESCRIPTOR** inputs,
    UINT32 output_count, APO_CONNECTION_DESCRIPTOR** outputs) {
    // The SDK base permits locking an uninitialized stateless APO. Contour's
    // lifecycle requires Initialize first, including for the standalone host.
    HRESULT result = !m_bIsInitialized ? APOERR_NOT_INITIALIZED :
        CBaseAudioProcessingObject::LockForProcess(input_count, inputs, output_count, outputs);
    if (SUCCEEDED(result)) {
        try { if (post_mix_) { prepare_runtime(); } }
        catch (...) {
            stop_worker(); live_eq_.clear();
            CBaseAudioProcessingObject::UnlockForProcess();
            result = E_FAIL;
        }
        process_calls_ = 0;
        processed_frames_ = 0;
        ratio_observed_ = 0;
        last_ratio_bits_ = 0;
    }
    TraceLoggingWrite(diagnostic_provider, "LockForProcess",
        TraceLoggingGuid(m_pRegProperties->clsid, "ClassId"), TraceLoggingPointer(this, "Instance"),
        TraceLoggingHResult(result, "Result"), TraceLoggingUInt32(m_u32SamplesPerFrame, "Channels"));
    return result;
}

void GainApo::report_processing_summary(const char* phase) const {
    // Lifecycle callers have stopped processing before unlocking/destroying.
    TraceLoggingWrite(diagnostic_provider, "ProcessingSummary",
        TraceLoggingGuid(m_pRegProperties->clsid, "ClassId"), TraceLoggingPointer(this, "Instance"),
        TraceLoggingString(phase, "Phase"), TraceLoggingInt64(static_cast<LONG64>(process_calls_), "Calls"),
        TraceLoggingInt64(static_cast<LONG64>(processed_frames_), "Frames"), TraceLoggingBoolean(ratio_observed_ != 0, "RatioObserved"),
        TraceLoggingUInt64(live_eq_.applied_revision(), "AppliedRevision"),
        TraceLoggingFloat32(std::bit_cast<float>(static_cast<LONG>(last_ratio_bits_)), "LastGainRatio"));
}

HRESULT GainApo::UnlockForProcess() {
    const HRESULT result = CBaseAudioProcessingObject::UnlockForProcess();
    if (SUCCEEDED(result)) { stop_worker(); report_processing_summary("Unlock"); live_eq_.clear(); }
    return result;
}

HRESULT GainApo::Reset() {
    const auto result = CBaseAudioProcessingObject::Reset();
    if (SUCCEEDED(result)) { live_eq_.reset(); }
    return result;
}

void GainApo::stop_worker() noexcept {
    if (worker_) {
        SetEvent(worker_stop_);
        WaitForSingleObject(worker_, INFINITE);
        CloseHandle(worker_); worker_ = nullptr;
    }
    if (worker_stop_) { CloseHandle(worker_stop_); worker_stop_ = nullptr; }
}

void GainApo::prepare_runtime() {
    stop_worker();
    profile_revision_ = 0; seen_profile_.clear(); last_profile_error_.clear();
    // Preserve the already-verified -6 dB lab behavior until a profile is
    // explicitly supplied. This is a lab default, not an extra gain on PEQ.
    eq::Layer fallback; fallback.gain_db = -6;
    auto* initial = eq::ResidentBank::prepare({{fallback}}, sample_rate_, channels_, 0);
    live_eq_.initialize(initial, static_cast<std::uint32_t>(sample_rate_*0.020));
    if (!system_effect_) { return; }
    wchar_t filename[32768]{};
    const auto size = GetModuleFileNameW(module_instance, filename, 32768);
    if (size == 0 || size >= 32768) { throw std::runtime_error("Cannot locate APO package"); }
    profile_path_ = std::filesystem::path(filename).parent_path()/L"profile.contour";
    try {
        const auto text = eq::read_profile_file(profile_path_);
        if (text) {
            seen_profile_ = *text;
            const auto profile = eq::parse_profile(*text);
            if (profile.channels != channels_) { throw std::invalid_argument("Profile channel layout does not match endpoint"); }
            auto* bank = eq::ResidentBank::prepare(profile.configuration, sample_rate_, channels_, 1);
            live_eq_.initialize(bank, static_cast<std::uint32_t>(sample_rate_*0.020));
            profile_revision_ = 1;
            TraceLoggingWrite(diagnostic_provider, "ProfileLoaded", TraceLoggingUInt64(profile_revision_, "Revision"));
        }
    } catch (const std::exception& error) {
        last_profile_error_ = error.what();
        TraceLoggingWrite(diagnostic_provider, "ProfileRejected", TraceLoggingString(error.what(), "Reason"));
    }
    worker_stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!worker_stop_) { throw std::runtime_error("Cannot create profile worker stop event"); }
    worker_ = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, worker_entry, this, 0, nullptr));
    if (!worker_) { throw std::runtime_error("Cannot create profile worker"); }
}

void GainApo::check_profile() {
    live_eq_.reclaim();
    const auto text = eq::read_profile_file(profile_path_);
    if (!text || *text == seen_profile_) { return; }
    seen_profile_ = *text;
    const auto profile = eq::parse_profile(*text);
    if (profile.channels != channels_) { throw std::invalid_argument("Profile channel layout does not match endpoint"); }
    auto* bank = eq::ResidentBank::prepare(profile.configuration, sample_rate_, channels_, profile_revision_+1);
    if (!live_eq_.publish(bank)) { throw std::invalid_argument("Prepared bank layout mismatch"); }
    ++profile_revision_;
    last_profile_error_.clear();
    TraceLoggingWrite(diagnostic_provider, "ProfileQueued", TraceLoggingUInt64(profile_revision_, "Revision"));
}

unsigned __stdcall GainApo::worker_entry(void* context) noexcept {
    auto& self = *static_cast<GainApo*>(context);
    auto observed = self.live_eq_.applied_revision();
    while (WaitForSingleObject(self.worker_stop_, 100) == WAIT_TIMEOUT) {
        try { self.check_profile(); }
        catch (const std::exception& error) {
            // Diagnostics are outside the audio callback; repeated bad files
            // are not repeatedly parsed or reported.
            try {
                if (self.last_profile_error_ != error.what()) {
                    self.last_profile_error_ = error.what();
                    TraceLoggingWrite(diagnostic_provider, "ProfileRejected", TraceLoggingString(error.what(), "Reason"));
                }
            } catch (...) {} // Low-memory diagnostics must not terminate audiodg.
        } catch (...) {}
        const auto applied = self.live_eq_.applied_revision();
        if (applied != observed) {
            observed = applied;
            TraceLoggingWrite(diagnostic_provider, "ProfileApplied", TraceLoggingUInt64(applied, "Revision"));
        }
    }
    return 0;
}

HRESULT GainApo::ValidateAndCacheConnectionInfo(UINT32 input_count,
    APO_CONNECTION_DESCRIPTOR** inputs, UINT32 output_count,
    APO_CONNECTION_DESCRIPTOR** outputs) {
    // Validate our one-input/one-output shape before accessing descriptor data;
    // the SDK base also checks its supported formats and lock state.
    if (input_count != 1 || output_count != 1) {
        return APOERR_NUM_CONNECTIONS_INVALID;
    }
    if (inputs == nullptr || outputs == nullptr || inputs[0] == nullptr ||
        outputs[0] == nullptr || inputs[0]->pFormat == nullptr || outputs[0]->pFormat == nullptr) {
        return E_POINTER;
    }
    UNCOMPRESSEDAUDIOFORMAT input{};
    UNCOMPRESSEDAUDIOFORMAT output{};
    if (FAILED(inputs[0]->pFormat->GetUncompressedAudioFormat(&input)) ||
        FAILED(outputs[0]->pFormat->GetUncompressedAudioFormat(&output)) ||
        !supported_format(input) || !supported_format(output) ||
        !matching_formats(input, output)) {
        return APOERR_INVALID_CONNECTION_FORMAT;
    }
    // APO_FLAG_INPLACE disables the SDK base's overlap rejection altogether.
    // Our forward sample loop supports identical buffers, not offset overlap.
    const auto input_address = inputs[0]->pBuffer;
    const auto output_address = outputs[0]->pBuffer;
    const auto input_bytes = static_cast<std::uint64_t>(inputs[0]->u32MaxFrameCount) *
        input.dwSamplesPerFrame * sizeof(float);
    const auto output_bytes = static_cast<std::uint64_t>(outputs[0]->u32MaxFrameCount) *
        output.dwSamplesPerFrame * sizeof(float);
    if (input_address != 0 && output_address != 0 && input_address != output_address &&
        ((input_address < output_address && output_address - input_address < input_bytes) ||
         (output_address < input_address && input_address - output_address < output_bytes))) {
        return APOERR_BUFFERS_OVERLAP;
    }
    const HRESULT result = CBaseAudioProcessingObject::ValidateAndCacheConnectionInfo(
        input_count, inputs, output_count, outputs);
    if (FAILED(result)) {
        return result;
    }
    std::array<float, dsp::max_channels> gains{};
    gains.fill(gain_);
    if (!processor_.configure(std::span(gains.data(), input.dwSamplesPerFrame))) {
        return E_UNEXPECTED;
    }
    max_input_frames_ = inputs[0]->u32MaxFrameCount;
    max_output_frames_ = outputs[0]->u32MaxFrameCount;
    sample_rate_ = input.fFramesPerSecond;
    channels_ = input.dwSamplesPerFrame;
    return S_OK;
}

#pragma AVRT_CODE_BEGIN
void GainApo::APOProcess(UINT32 input_count, APO_CONNECTION_PROPERTY** inputs,
    UINT32 output_count, APO_CONNECTION_PROPERTY** outputs) {
    if (output_count != 1 || outputs == nullptr || outputs[0] == nullptr) {
        return;
    }
    auto& output = *outputs[0];
    // Snapshot input first: the host may reuse the same connection property.
    const APO_CONNECTION_PROPERTY input =
        (input_count == 1 && inputs != nullptr && inputs[0] != nullptr)
        ? *inputs[0] : APO_CONNECTION_PROPERTY{};
    output.u32ValidFrameCount = 0;
    output.u32BufferFlags = BUFFER_INVALID;
    if (!m_bIsLocked ||
        (input.u32Signature != APO_CONNECTION_PROPERTY_SIGNATURE &&
         input.u32Signature != APO_CONNECTION_PROPERTY_V2_SIGNATURE) ||
        input.u32ValidFrameCount > max_input_frames_ ||
        input.u32ValidFrameCount > max_output_frames_ ||
        (input.u32BufferFlags != BUFFER_VALID && input.u32BufferFlags != BUFFER_SILENT)) {
        return;
    }
    auto output_flags = input.u32BufferFlags;
    if (input.u32ValidFrameCount != 0 && (post_mix_ || input.u32BufferFlags == BUFFER_VALID)) {
        if ((input.u32BufferFlags == BUFFER_VALID && input.pBuffer == 0) || output.pBuffer == 0) {
            return;
        }
        const auto* source = input.u32BufferFlags == BUFFER_SILENT ? nullptr : reinterpret_cast<const float*>(input.pBuffer);
        auto* destination = reinterpret_cast<float*>(output.pBuffer);
        const float first = source ? source[0] : 0; // Before in-place processing; never read logical silence.
        if (post_mix_) {
            const bool audible = live_eq_.process(source, destination, input.u32ValidFrameCount);
            output_flags = audible ? BUFFER_VALID : BUFFER_SILENT;
        } else { processor_.process(source, destination, input.u32ValidFrameCount); }
        if (first > 1.0e-9F || first < -1.0e-9F) {
            InterlockedExchange(&last_ratio_bits_, std::bit_cast<LONG>(destination[0] / first));
            InterlockedExchange(&ratio_observed_, 1);
        }
    }
    InterlockedIncrement64(&process_calls_);
    InterlockedAdd64(&processed_frames_, input.u32ValidFrameCount);
    // Silent input can emit a PEQ tail; silence is determined after processing.
    output.u32ValidFrameCount = input.u32ValidFrameCount;
    output.u32BufferFlags = output_flags;
}
#pragma AVRT_CODE_END

} // namespace contour::apo
