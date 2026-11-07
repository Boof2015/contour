#include <windows.h>
#include <audioenginebaseapo.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <contour/apo_ids.h>
#include <contour/profile.h>
#include "test_support.h"
#include "control_test_support.h"
#include <contour/endpoint_session.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <malloc.h>
#include <memory>
#include <string_view>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using test::expect_near;
using test::require;
using namespace contour::apo;

CLSID test_pre_clsid = pre_mix_clsid;
CLSID test_post_clsid = post_mix_clsid;
bool test_component = false;
double default_db = -6.0;
double default_gain() { return std::pow(10.0, default_db / 20.0); }

void hr(HRESULT actual, HRESULT expected, std::string_view operation) {
    if (actual != expected) {
        std::ostringstream message;
        message << operation << ": expected HRESULT 0x" << std::hex
                << static_cast<unsigned long>(expected) << ", got 0x"
                << static_cast<unsigned long>(actual);
        throw std::runtime_error(message.str());
    }
}

struct ComRuntime {
    ComRuntime() { hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED), S_OK, "COM initialization"); }
    ~ComRuntime() { CoUninitialize(); }
};

class Module {
public:
    using GetClassObject = HRESULT (STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    using CanUnloadNow = HRESULT (STDAPICALLTYPE*)();

    explicit Module(const std::filesystem::path& path) {
        const auto absolute = std::filesystem::absolute(path);
        handle_ = LoadLibraryExW(absolute.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (handle_ == nullptr) {
            throw std::runtime_error("Could not load APO DLL; Windows error " +
                std::to_string(GetLastError()));
        }
        const auto factory_export = GetProcAddress(handle_, "DllGetClassObject");
        const auto unload_export = GetProcAddress(handle_, "DllCanUnloadNow");
        if (factory_export == nullptr || unload_export == nullptr) {
            FreeLibrary(handle_);
            throw std::runtime_error("APO DLL is missing COM exports");
        }
        get_class = std::bit_cast<GetClassObject>(factory_export);
        can_unload = std::bit_cast<CanUnloadNow>(unload_export);
    }
    ~Module() { FreeLibrary(handle_); }
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
    HMODULE handle() const noexcept { return handle_; }

    ComPtr<IClassFactory> factory(REFCLSID id) const {
        ComPtr<IClassFactory> result;
        hr(get_class(id, IID_PPV_ARGS(&result)), S_OK, "DllGetClassObject");
        return result;
    }
    ComPtr<IAudioProcessingObject> create(REFCLSID id) const {
        const auto creator = factory(id);
        ComPtr<IAudioProcessingObject> result;
        hr(creator->CreateInstance(nullptr, IID_PPV_ARGS(&result)), S_OK, "CreateInstance");
        return result;
    }

    GetClassObject get_class = nullptr;
    CanUnloadNow can_unload = nullptr;
private:
    HMODULE handle_ = nullptr;
};

constexpr GUID float_format = {STATIC_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT};
constexpr GUID pcm_format = {STATIC_KSDATAFORMAT_SUBTYPE_PCM};

ComPtr<IAudioMediaType> format(UINT32 channels = 2, float rate = 48000.0F,
                             bool integer = false, DWORD mask = 0) {
    UNCOMPRESSEDAUDIOFORMAT description{};
    description.guidFormatType = integer ? pcm_format : float_format;
    description.dwSamplesPerFrame = channels;
    description.dwBytesPerSampleContainer = integer ? 2 : 4;
    description.dwValidBitsPerSample = integer ? 16 : 32;
    description.fFramesPerSecond = rate;
    description.dwChannelMask = mask;
    ComPtr<IAudioMediaType> result;
    hr(CreateAudioMediaTypeFromUncompressedAudioFormat(&description, &result), S_OK,
       "create audio media type");
    return result;
}

struct AlignedFree {
    void operator()(float* memory) const noexcept { _aligned_free(memory); }
};

class Buffer {
public:
    explicit Buffer(std::size_t samples) : samples_(samples),
        storage_(static_cast<float*>(_aligned_malloc((samples + 8) * sizeof(float), 16))) {
        if (!storage_) {
            throw std::bad_alloc();
        }
        std::fill_n(storage_.get(), samples + 8, sentinel);
    }
    float* data() const { return storage_.get() + 4; }
    void fill(float value) { std::fill_n(data(), samples_, value); }
    void guards() const {
        for (std::size_t i = 0; i < 4; ++i) {
            require(storage_.get()[i] == sentinel && data()[samples_ + i] == sentinel,
                    "buffer guard overwritten");
        }
    }
    static constexpr float sentinel = 12345.0F;
private:
    std::size_t samples_;
    std::unique_ptr<float, AlignedFree> storage_;
};

APO_CONNECTION_DESCRIPTOR descriptor(IAudioMediaType* type, float* data, UINT32 frames) {
    return {APO_CONNECTION_BUFFER_TYPE_EXTERNAL, reinterpret_cast<UINT_PTR>(data), frames,
            type, APO_CONNECTION_DESCRIPTOR_SIGNATURE};
}

APO_CONNECTION_PROPERTY property(float* data, UINT32 frames, APO_BUFFER_FLAGS flags) {
    return {reinterpret_cast<UINT_PTR>(data), frames, flags, APO_CONNECTION_PROPERTY_SIGNATURE};
}

class Instance {
public:
    Instance(const Module& module, REFCLSID id, bool initialize = true)
        : Instance(module.create(id), initialize) {}
    explicit Instance(ComPtr<IAudioProcessingObject> object, bool initialize = true)
        : apo(std::move(object)) {
        hr(apo.As(&configuration), S_OK, "configuration interface");
        hr(apo.As(&rt), S_OK, "real-time interface");
        if (initialize) {
            hr(apo->Initialize(0, nullptr), S_OK, "APO Initialize");
        }
    }
    ~Instance() {
        if (locked_) {
            configuration->UnlockForProcess();
        }
    }
    HRESULT try_lock(APO_CONNECTION_DESCRIPTOR& input, APO_CONNECTION_DESCRIPTOR& output) {
        auto* in = &input;
        auto* out = &output;
        const HRESULT result = configuration->LockForProcess(1, &in, 1, &out);
        if (result == S_OK) {
            locked_ = true;
        }
        return result;
    }
    void lock(APO_CONNECTION_DESCRIPTOR& input, APO_CONNECTION_DESCRIPTOR& output) {
        hr(try_lock(input, output), S_OK, "LockForProcess");
    }
    void unlock() {
        hr(configuration->UnlockForProcess(), S_OK, "UnlockForProcess");
        locked_ = false;
    }
    void process(APO_CONNECTION_PROPERTY& input, APO_CONNECTION_PROPERTY& output) {
        auto* in = &input;
        auto* out = &output;
        rt->APOProcess(1, &in, 1, &out);
    }

    ComPtr<IAudioProcessingObject> apo;
    ComPtr<IAudioProcessingObjectConfiguration> configuration;
    ComPtr<IAudioProcessingObjectRT> rt;
private:
    bool locked_ = false;
};

void test_identity(const Module& module) {
    require(component_pre_mix_clsid != pre_mix_clsid && component_pre_mix_clsid != post_mix_clsid &&
        component_post_mix_clsid != pre_mix_clsid && component_post_mix_clsid != post_mix_clsid &&
        component_pre_mix_clsid != component_post_mix_clsid, "new registration must use distinct identities");
    hr(module.can_unload(), S_OK, "initially unloadable");
    ComPtr<IClassFactory> unknown;
    hr(module.get_class(GUID_NULL, IID_PPV_ARGS(&unknown)), CLASS_E_CLASSNOTAVAILABLE,
       "unknown class");
    hr(module.get_class(test_pre_clsid, __uuidof(IClassFactory), nullptr), E_POINTER,
       "null factory output");
    for (const auto& id : {test_pre_clsid, test_post_clsid}) {
        auto factory = module.factory(id);
        hr(module.can_unload(), S_FALSE, "factory keeps DLL loaded");
        hr(factory->LockServer(TRUE), S_OK, "lock server");
        factory.Reset();
        hr(module.can_unload(), S_FALSE, "server lock survives factory");
        factory = module.factory(id);
        hr(factory->LockServer(FALSE), S_OK, "unlock server");
        auto apo = module.create(id);
        factory.Reset();
        hr(module.can_unload(), S_FALSE, "instance keeps DLL loaded");
        ComPtr<IAudioSystemEffects> effects;
        hr(apo.As(&effects), S_OK, "system effect marker");
        ComPtr<IUnknown> via_apo;
        ComPtr<IUnknown> via_effects;
        hr(apo.As(&via_apo), S_OK, "IUnknown from APO");
        hr(effects.As(&via_effects), S_OK, "IUnknown from system effects");
        require(via_apo.Get() == via_effects.Get(), "stable COM identity");
        ComPtr<IClassFactory> unsupported;
        hr(apo.As(&unsupported), E_NOINTERFACE, "unsupported interface");
        APO_REG_PROPERTIES* raw = nullptr;
        hr(apo->GetRegistrationProperties(&raw), S_OK, "registration properties");
        const std::unique_ptr<APO_REG_PROPERTIES, decltype(&CoTaskMemFree)> properties(raw, CoTaskMemFree);
        require(properties->clsid == id, "correct class ID");
        require((properties->Flags & APO_FLAG_INPLACE) != 0, "in-place advertised");
        require(properties->u32MaxInputConnections == 1 && properties->u32MaxOutputConnections == 1,
                "one input and output");
    }
    hr(module.can_unload(), S_OK, "all references released");
}

// Stack-owned outer object models the engine's controlling IUnknown. Public
// APO interfaces must count references here; only the private inner identity
// may own the APO itself.
class AggregateHost final : public IUnknown {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (result == nullptr) { return E_POINTER; }
        *result = nullptr;
        if (iid == __uuidof(IUnknown)) {
            *result = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        if (iid == __uuidof(IAudioProcessingObject) || iid == __uuidof(IAudioProcessingObjectRT) ||
            iid == __uuidof(IAudioProcessingObjectConfiguration) || iid == __uuidof(IAudioSystemEffects)) {
            return inner ? inner->QueryInterface(iid, result) : E_NOINTERFACE;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }
    ULONG references = 1;
    ComPtr<IUnknown> inner;
};

void test_aggregation(const Module& module) {
    for (const auto& id : {test_pre_clsid, test_post_clsid}) {
        AggregateHost outer;
        auto factory = module.factory(id);
        ComPtr<IAudioProcessingObject> invalid;
        hr(factory->CreateInstance(&outer, IID_PPV_ARGS(&invalid)), E_NOINTERFACE,
           "aggregation creation requires IUnknown");
        require(!invalid && outer.references == 1, "failed aggregation has no references");
        hr(factory->CreateInstance(&outer, IID_PPV_ARGS(&outer.inner)), S_OK, "create aggregate");
        factory.Reset();
        require(outer.references == 1, "controlling identity is borrowed");
        hr(module.can_unload(), S_FALSE, "inner identity owns APO lifetime");
        ComPtr<IUnknown> inner_identity;
        hr(outer.inner.As(&inner_identity), S_OK, "private identity query");
        require(inner_identity.Get() == outer.inner.Get() && outer.references == 1,
                "private identity is nondelegating");
        inner_identity.Reset();
        {
            ComPtr<IAudioProcessingObject> apo;
            hr(outer.QueryInterface(IID_PPV_ARGS(&apo)), S_OK, "outer exposes APO");
            require(outer.references == 2, "public APO reference belongs to outer");
            ComPtr<IUnknown> identity;
            hr(apo.As(&identity), S_OK, "public identity");
            require(identity.Get() == static_cast<IUnknown*>(&outer), "APO delegates identity to outer");
            ComPtr<IAudioSystemEffects> effects;
            hr(apo.As(&effects), S_OK, "aggregate interface navigation");
            ComPtr<IUnknown> effects_identity;
            hr(effects.As(&effects_identity), S_OK, "effects identity");
            require(effects_identity.Get() == identity.Get(), "all public interfaces share outer identity");
            ComPtr<IClassFactory> unsupported;
            hr(apo.As(&unsupported), E_NOINTERFACE, "outer rejects unsupported interface");
            Instance instance(apo, false);
            // A staged installation may contain any fallback profile. Exercise
            // aggregation against explicit endpoint settings, not adjacent files.
            const auto store = Microsoft::WRL::Make<control_test::Store>();
            const auto persisted = Microsoft::WRL::Make<control_test::Store>();
            contour::eq::write_saved_profile(*persisted.Get(),
                {{{{"Aggregate test", {}, {}, default_db}}}, 48000, 2});
            const auto endpoint = Microsoft::WRL::Make<control_test::Endpoint>(store.Get(), persisted.Get());
            const auto devices = Microsoft::WRL::Make<control_test::Devices>(endpoint.Get());
            APOInitSystemEffects init{};
            init.APOInit.cbSize = sizeof(init);
            init.APOInit.clsid = id;
            init.pDeviceCollection = devices.Get();
            hr(apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init)), S_OK, "aggregate initialize");
            auto media = format();
            Buffer input(16), output(16);
            input.fill(0.25F);
            auto in = descriptor(media.Get(), input.data(), 8);
            auto out = descriptor(media.Get(), output.data(), 8);
            instance.lock(in, out);
            auto source = property(input.data(), 8, BUFFER_VALID);
            auto target = property(output.data(), 0, BUFFER_INVALID);
            instance.process(source, target);
            const double gain = id == test_post_clsid ? default_gain() : 1.0;
            for (int i = 0; i < 16; ++i) { expect_near(output.data()[i], 0.25 * gain, "aggregate audio"); }
            instance.unlock();
            input.guards();
            output.guards();
        }
        require(outer.references == 1, "all delegated references released");
        outer.inner.Reset();
        hr(module.can_unload(), S_OK, "aggregate releases inner lifetime");
    }
}

void test_initialization(const Module& module) {
    for (const auto& id : {test_pre_clsid, test_post_clsid}) {
        Instance instance(module, id, false);
        BYTE dummy = 0;
        hr(instance.apo->Initialize(1, nullptr), E_INVALIDARG, "missing init data");
        hr(instance.apo->Initialize(0, &dummy), E_INVALIDARG, "unexpected init data");
        APOInitSystemEffects init{};
        init.APOInit.cbSize = sizeof(init);
        init.APOInit.clsid = GUID_NULL;
        hr(instance.apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init)),
           APOERR_INVALID_APO_CLSID, "wrong init identity");
        init.APOInit.clsid = id;
        init.APOInit.cbSize = 1;
        hr(instance.apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init)),
           E_INVALIDARG, "wrong init size");
        init.APOInit.cbSize = sizeof(init);
        hr(instance.apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init)), S_OK,
           "legacy system-effects initialization");
        hr(instance.apo->Initialize(0, nullptr), APOERR_ALREADY_INITIALIZED, "double initialize");
        HNSTIME latency = -1;
        hr(instance.apo->GetLatency(&latency), S_OK, "latency");
        require(latency == 0, "no algorithmic latency");
        for (const UINT32 frames : {0U, 1U, 257U, 4096U}) {
            require(instance.rt->CalcInputFrames(frames) == frames &&
                    instance.rt->CalcOutputFrames(frames) == frames, "one-to-one frame counts");
        }
    }
}

void test_negotiation(const Module& module) {
    Instance instance(module, test_post_clsid);
    for (const auto channels : {1U, 2U, 6U, 8U, 32U}) {
        for (const float rate : {44100.0F, 48000.0F, 96000.0F, 192000.0F}) {
            auto media = format(channels, rate);
            ComPtr<IAudioMediaType> supported;
            hr(instance.apo->IsInputFormatSupported(nullptr, media.Get(), &supported), S_OK,
               "supported float input");
            require(supported.Get() == media.Get(), "exact requested format returned");
            hr(instance.apo->IsOutputFormatSupported(media.Get(), media.Get(), nullptr), S_OK,
               "supported float output, optional result");
        }
    }
    auto stereo = format(2, 48000.0F, false, 3);
    auto pcm = format(2, 48000.0F, true, 3);
    ComPtr<IAudioMediaType> suggested;
    hr(instance.apo->IsInputFormatSupported(nullptr, pcm.Get(), &suggested), S_FALSE,
       "integer gets float suggestion");
    UNCOMPRESSEDAUDIOFORMAT description{};
    hr(suggested->GetUncompressedAudioFormat(&description), S_OK, "suggested format");
    require(description.guidFormatType == float_format && description.dwValidBitsPerSample == 32 &&
        description.dwSamplesPerFrame == 2 && description.fFramesPerSecond == 48000.0F &&
        description.dwChannelMask == 3, "float suggestion preserves layout and rate");
    for (const auto& mismatch : {format(1), format(2, 44100.0F), format(2, 48000.0F, false, 12), pcm}) {
        suggested.Reset();
        hr(instance.apo->IsInputFormatSupported(stereo.Get(), mismatch.Get(), &suggested), S_FALSE,
           "input mismatch gets opposite format");
        require(suggested.Get() == stereo.Get(), "matching suggestion");
        hr(instance.apo->IsOutputFormatSupported(stereo.Get(), mismatch.Get(), nullptr), S_FALSE,
           "output mismatch detected");
    }
    auto excessive = format(33);
    hr(instance.apo->IsInputFormatSupported(nullptr, excessive.Get(), nullptr),
       APOERR_FORMAT_NOT_SUPPORTED, "too many channels");
    auto below_peq_range = format(2,4000.0F);
    hr(instance.apo->IsInputFormatSupported(nullptr,below_peq_range.Get(),nullptr),
       APOERR_FORMAT_NOT_SUPPORTED,"rate outside PEQ design range");
    hr(instance.apo->IsInputFormatSupported(pcm.Get(), stereo.Get(), nullptr),
       APOERR_FORMAT_NOT_SUPPORTED, "unsupported opposite format");
    hr(instance.apo->IsInputFormatSupported(nullptr, nullptr, nullptr), E_POINTER, "missing request");
}

void test_lifecycle(const Module& module) {
    Instance instance(module, test_post_clsid, false);
    auto media = format();
    Buffer input(64 * 2), output(64 * 2);
    auto in = descriptor(media.Get(), input.data(), 64);
    auto out = descriptor(media.Get(), output.data(), 64);
    hr(instance.try_lock(in, out), APOERR_NOT_INITIALIZED, "lock before initialization");
    hr(instance.apo->Initialize(0, nullptr), S_OK, "initialize");
    require(FAILED(instance.configuration->LockForProcess(0, nullptr, 0, nullptr)),
            "invalid connection counts rejected");
    auto integer = format(2, 48000.0F, true);
    auto invalid = descriptor(integer.Get(), input.data(), 64);
    require(FAILED(instance.try_lock(invalid, out)), "integer lock rejected");
    for (const auto& mismatch : {format(1), format(2, 44100.0F), format(2, 48000.0F, false, 3)}) {
        invalid = descriptor(mismatch.Get(), input.data(), 64);
        require(FAILED(instance.try_lock(invalid, out)), "mismatched lock rejected");
    }
    auto short_output = descriptor(media.Get(), output.data(), 32);
    require(FAILED(instance.try_lock(in, short_output)), "insufficient output capacity rejected");
    auto overlap = descriptor(media.Get(), input.data() + 4, 60);
    auto shorter_input = descriptor(media.Get(), input.data(), 60);
    require(FAILED(instance.try_lock(shorter_input, overlap)), "partial overlap rejected");
    instance.lock(in, out);
    hr(instance.try_lock(in, out), APOERR_APO_LOCKED, "double lock rejected");
    UINT32 channels = 0;
    hr(instance.apo->GetInputChannelCount(&channels), S_OK, "channel count");
    require(channels == 2, "locked channel count");
    hr(instance.apo->Reset(), S_OK, "stateless reset while locked");
    instance.unlock();
    hr(instance.configuration->UnlockForProcess(), APOERR_ALREADY_UNLOCKED, "double unlock");
    // Re-lock the same instance with a different format: no cached stereo stride.
    auto mono = format(1, 96000.0F);
    in = descriptor(mono.Get(), input.data(), 64);
    out = descriptor(mono.Get(), output.data(), 64);
    instance.lock(in, out);
    input.fill(1.0F);
    auto source = property(input.data(), 64, BUFFER_VALID);
    auto target = property(output.data(), 0, BUFFER_INVALID);
    instance.process(source, target);
    expect_near(output.data()[63], default_gain(), "relocked mono gain");
    require(output.data()[64] == Buffer::sentinel, "relocked mono frame boundary");
    input.guards();
    output.guards();
}

void test_processing(const Module& module, REFCLSID id, bool inplace) {
    constexpr UINT32 capacity = 257;
    const double gain = id == test_pre_clsid ? 1.0 : default_gain();
    for (const UINT32 channels : {1U, 2U, 6U, 8U, 32U}) {
        auto media = format(channels);
        Buffer input(capacity * channels), output(capacity * channels);
        Instance instance(module, id);
        float* destination = inplace ? input.data() : output.data();
        auto in = descriptor(media.Get(), input.data(), capacity);
        auto out = descriptor(media.Get(), destination, capacity);
        instance.lock(in, out);
        for (UINT32 block = 0; block < 128; ++block) {
            const UINT32 frames = std::array{0U, 1U, 7U, 64U, capacity}[block % 5];
            input.fill(Buffer::sentinel);
            output.fill(Buffer::sentinel);
            std::vector<float> original(frames * channels);
            for (std::size_t i = 0; i < original.size(); ++i) {
                original[i] = static_cast<float>(static_cast<int>((i * 37 + block * 11) % 127) - 63) / 64.0F;
                input.data()[i] = original[i];
            }
            auto source = property(input.data(), frames, BUFFER_VALID);
            auto target = property(destination, 999, BUFFER_SILENT);
            instance.process(source, target);
            require(target.u32ValidFrameCount == frames && target.u32BufferFlags == BUFFER_VALID,
                    "output frame count and valid flag");
            for (std::size_t i = 0; i < original.size(); ++i) {
                expect_near(destination[i], static_cast<double>(original[i]) * gain, "processed sample");
                if (!inplace) {
                    require(input.data()[i] == original[i], "separate input unchanged");
                }
            }
            for (std::size_t i = original.size(); i < capacity * channels; ++i) {
                require(destination[i] == Buffer::sentinel, "no writes past valid frames");
            }
            input.guards();
            output.guards();
        }
    }
}

void test_flags(const Module& module) {
    Instance instance(module, test_post_clsid);
    auto media = format();
    Buffer input(32), output(32);
    auto in = descriptor(media.Get(), input.data(), 16);
    auto out = descriptor(media.Get(), output.data(), 16);
    instance.lock(in, out);
    input.fill(std::numeric_limits<float>::quiet_NaN());
    auto source = property(input.data(), 16, BUFFER_SILENT);
    auto target = property(output.data(), 0, BUFFER_INVALID);
    instance.process(source, target);
    require(target.u32BufferFlags == BUFFER_SILENT && target.u32ValidFrameCount == 16,
            "logical silence propagated");
    require(output.data()[0] == 0, "logical silence ignores stale NaN input storage");
    source.u32BufferFlags = BUFFER_INVALID;
    instance.process(source, target);
    require(target.u32BufferFlags == BUFFER_INVALID && target.u32ValidFrameCount == 0,
            "invalid input produces no frames");
    source.u32BufferFlags = BUFFER_VALID;
    source.u32ValidFrameCount = 17;
    instance.process(source, target);
    require(target.u32BufferFlags == BUFFER_INVALID && target.u32ValidFrameCount == 0,
            "oversized frame count contained");
    source.u32ValidFrameCount = 0;
    source.pBuffer = 0;
    instance.process(source, target);
    require(target.u32ValidFrameCount == 0 && target.u32BufferFlags == BUFFER_VALID,
            "zero frames need no sample pointer");
    source = property(input.data(), 1, BUFFER_VALID);
    input.data()[0] = 0.0F;
    input.data()[1] = 1.0F;
    source.u32Signature = APO_CONNECTION_PROPERTY_V2_SIGNATURE;
    // Only the common prefix is accessed; use a full v2 object for the call.
    APO_CONNECTION_PROPERTY_V2 version2{source, 0};
    instance.process(version2.property, target);
    expect_near(output.data()[0], 0.0, "no right-to-left crosstalk");
    expect_near(output.data()[1], default_gain(), "right channel impulse");
    instance.unlock();
    instance.process(source, target);
    require(target.u32BufferFlags == BUFFER_INVALID && target.u32ValidFrameCount == 0,
            "unlocked APO refuses processing");
    input.guards();
    output.guards();
}

void test_chain(const Module& module) {
    constexpr UINT32 frames = 480;
    auto media = format(2, 48000.0F, false, 3);
    Buffer input(frames * 2), intermediate(frames * 2), output(frames * 2);
    Instance pre(module, test_pre_clsid), post(module, test_post_clsid);
    auto a = descriptor(media.Get(), input.data(), frames);
    auto b = descriptor(media.Get(), intermediate.data(), frames);
    auto c = descriptor(media.Get(), output.data(), frames);
    pre.lock(a, b);
    post.lock(b, c);
    double input_energy[2]{};
    double output_energy[2]{};
    for (UINT32 frame = 0; frame < frames; ++frame) {
        input.data()[2 * frame] = static_cast<float>(0.5 * std::sin(frame * 0.1308996938995747));
        input.data()[2 * frame + 1] = static_cast<float>(0.25 * std::cos(frame * 0.06544984694978735));
    }
    auto source = property(input.data(), frames, BUFFER_VALID);
    auto middle = property(intermediate.data(), 0, BUFFER_INVALID);
    auto target = property(output.data(), 0, BUFFER_INVALID);
    pre.process(source, middle);
    post.process(middle, target);
    require(target.u32ValidFrameCount == frames && target.u32BufferFlags == BUFFER_VALID,
            "chain output valid");
    for (UINT32 frame = 0; frame < frames; ++frame) {
        for (UINT32 channel = 0; channel < 2; ++channel) {
            const auto index = frame * 2 + channel;
            const double sample = input.data()[index];
            const double actual = output.data()[index];
            expect_near(intermediate.data()[index], sample, "pre-mix unity");
            expect_near(output.data()[index], sample * default_gain(), "chain gain once");
            input_energy[channel] += sample * sample;
            output_energy[channel] += actual * actual;
        }
    }
    for (UINT32 channel = 0; channel < 2; ++channel) {
        const double db = 10.0 * std::log10(output_energy[channel] / input_energy[channel]);
        require(std::abs(db - default_db) < 1.0e-5, "measured chain gain matches registration family");
        std::cout << "  Channel " << channel << " measured gain: " << std::fixed
                  << std::setprecision(6) << db << " dB\n";
    }
    input.guards();
    intermediate.guards();
    output.guards();
}

void test_live_profile(const std::filesystem::path& path) {
    using namespace contour::eq;
    // Isolate writable settings from the real build and any staged package.
    const auto directory = path.parent_path() / (L"profile-host-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()));
    require(std::filesystem::create_directory(directory), "create private profile test directory");
    struct Cleanup {
        std::filesystem::path directory;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove(directory/L"profile.contour", ignored);
            std::filesystem::remove(directory/L"contour-apo.dll", ignored);
            std::filesystem::remove(directory, ignored);
        }
    } cleanup{directory};
    std::filesystem::copy_file(path, directory/L"contour-apo.dll");
    const auto profile_path = directory/L"profile.contour";
    const Module module(directory/L"contour-apo.dll");
    const Configuration filters{{{"Left", ChannelSelection::selected(1), {{FilterType::peak,997,-6,2}}},
        {"Right", ChannelSelection::selected(2), {{FilterType::peak,1499,-3,2}}}, {"All",{}, {},-2}}};
    for (const float rate : {48000.0F, 96000.0F}) {
        save_profile_file(profile_path,{filters,48000,2});
        Instance instance(module,test_post_clsid,false);
        APOInitSystemEffects init{};
        init.APOInit = {sizeof(init),test_post_clsid};
        hr(instance.apo->Initialize(sizeof(init),reinterpret_cast<BYTE*>(&init)),S_OK,"profile initialize");
        auto media = format(2,rate);
        constexpr UINT32 frames = 64;
        Buffer input(frames*2), output(frames*2);
        auto in = descriptor(media.Get(),input.data(),frames), out = descriptor(media.Get(),output.data(),frames);
        instance.lock(in,out);
        auto source = property(input.data(),frames,BUFFER_VALID), target = property(output.data(),0,BUFFER_INVALID);
        if (test_component) {
            // An unprovisioned component must remain neutral, even when an old
            // package profile exists beside the DLL or changes while locked.
            std::fill_n(input.data(), frames * 2, 0.1F);
            for (const auto gain : {-31.0, -12.0}) {
                save_profile_file(profile_path, {Configuration{{{"Old lab EQ", {}, {}, gain}}}, rate, 2});
                Sleep(250);
                instance.process(source, target);
                for (UINT32 i = 0; i < frames * 2; ++i) {
                    expect_near(output.data()[i], 0.1, "component ignored legacy package profile");
                }
            }
            instance.unlock(); instance.lock(in, out);
            instance.process(source, target);
            expect_near(output.data()[0], 0.1, "component remains neutral after relock");
            instance.unlock(); input.guards(); output.guards();
            continue;
        }
        ParametricEq reference; reference.configure(filters,rate,2);
        std::array<float,frames*2> expected{};
        for (unsigned block=0; block<10; ++block) {
            for (UINT32 f=0; f<frames; ++f) {
                input.data()[2*f] = static_cast<float>(0.1*std::sin((block*frames+f)*0.13));
                input.data()[2*f+1] = static_cast<float>(0.1*std::cos((block*frames+f)*0.17));
            }
            reference.process(input.data(),expected.data(),frames);
            instance.process(source,target);
            require(target.u32BufferFlags == BUFFER_VALID, "initial PEQ output invalid");
            for (std::size_t i=0; i<expected.size(); ++i) { expect_near(output.data()[i],expected[i],"saved PEQ at actual stream rate"); }
        }
        input.fill(std::numeric_limits<float>::quiet_NaN());
        source.u32BufferFlags = BUFFER_SILENT;
        instance.process(source,target);
        require(target.u32BufferFlags == BUFFER_VALID,"silent input discarded filter tail");
        for (UINT32 i=0; i<frames*2; ++i) { require(std::isfinite(output.data()[i]),"read stale silent input"); }
        hr(instance.apo->Reset(),S_OK,"reset PEQ history");
        instance.process(source,target);
        require(target.u32BufferFlags == BUFFER_SILENT,"reset retained PEQ history");

        source.u32BufferFlags = BUFFER_VALID; input.fill(0.1F);
        const auto settle = [&](double db, bool expect_transition) {
            const double desired = 0.1*std::pow(10.0,db/20);
            const auto deadline = GetTickCount64()+3000;
            bool transition = false;
            while (GetTickCount64() < deadline) {
                instance.process(source,target);
                require(target.u32BufferFlags == BUFFER_VALID,"updated output invalid");
                for (UINT32 i=2; i<frames*2; i+=2) {
                    if (std::abs(output.data()[i]-output.data()[i-2]) > 1e-8) { transition = true; }
                }
                if (std::abs(output.data()[0]-desired) < 1e-7 && std::abs(output.data()[frames*2-1]-desired) < 1e-7) {
                    require(!expect_transition || transition,"live edit skipped transition");
                    return;
                }
                Sleep(1);
            }
            throw std::runtime_error("profile update did not reach expected output");
        };
        Profile changed{{{{"All",{}, {},-12}}},48000,2};
        save_profile_file(profile_path,changed); settle(-12,true);
        // A malformed update must leave the last working bank active.
        { std::ofstream invalid(profile_path,std::ios::binary|std::ios::trunc); invalid << "CONTOUR broken\n"; }
        Sleep(250);
        instance.process(source,target);
        expect_near(output.data()[0],0.1*std::pow(10.0,-12.0/20),"invalid edit preserved current profile");
        changed.configuration.bypass = true;
        save_profile_file(profile_path,changed); settle(0,true);
        changed.configuration.bypass = false;
        save_profile_file(profile_path,changed); settle(-12,true);
        instance.unlock();
        instance.lock(in,out);
        instance.process(source,target);
        expect_near(output.data()[0],0.1*std::pow(10.0,-12.0/20),"relock reloads saved profile without extra gain");
        instance.unlock(); input.guards(); output.guards();
    }
    hr(module.can_unload(),S_OK,"profile workers stopped before DLL unload");
}

void test_control_bridge(const std::filesystem::path& path) {
    using namespace contour::eq;
    const auto directory=path.parent_path()/(L"control-host-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    require(std::filesystem::create_directory(directory),"create private control test directory");
    struct Cleanup {
        std::filesystem::path directory;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove(directory/L"profile.contour",ignored);
            std::filesystem::remove(directory/L"contour-apo.dll",ignored);
            std::filesystem::remove(directory,ignored);
        }
    } cleanup{directory};
    std::filesystem::copy_file(path,directory/L"contour-apo.dll");
    const Module module(directory/L"contour-apo.dll");
    const auto profile_path=directory/L"profile.contour";
    const auto gains=[](double left,double right) -> Configuration {
        return {{{"Left",ChannelSelection::selected(1),{},left},{"Right",ChannelSelection::selected(2),{},right}}};
    };
    for (const float rate : {48000.0F,44100.0F}) {
        const Profile saved{gains(-2,-3),48000,2};
        // A different legacy file proves settings mode does not consume it.
        save_profile_file(profile_path,{gains(-31,-32),48000,2});
        const auto original=read_profile_file(profile_path);
        const auto store=Microsoft::WRL::Make<control_test::Store>();
        const auto persisted=Microsoft::WRL::Make<control_test::Store>();
        EditSession seed(saved); save_profile_settings(seed,*persisted.Get());
        const auto endpoint=Microsoft::WRL::Make<control_test::Endpoint>(store.Get(),persisted.Get());
        const auto devices=Microsoft::WRL::Make<control_test::Devices>(endpoint.Get());
        Instance instance(module,test_post_clsid,false);
        APOInitSystemEffects init{}; init.APOInit={sizeof(init),test_post_clsid}; init.pDeviceCollection=devices.Get();
        hr(instance.apo->Initialize(sizeof(init),reinterpret_cast<BYTE*>(&init)),S_OK,"initialize settings bridge");
        constexpr UINT32 frames=128;
        auto media=format(2,rate); Buffer input(frames*2),output(frames*2); input.fill(0.1F);
        auto in=descriptor(media.Get(),input.data(),frames),out=descriptor(media.Get(),output.data(),frames);
        auto source=property(input.data(),frames,BUFFER_VALID),target=property(output.data(),0,BUFFER_INVALID);
        instance.lock(in,out);
        const auto wait_for=[&](auto condition) {
            const auto deadline=GetTickCount64()+4000;
            while (GetTickCount64()<deadline) { if (condition()) { return; } Sleep(1); }
            throw std::runtime_error("APO control bridge timed out");
        };
        const auto status=[&]() -> std::optional<PreviewStatus> {
            const auto bytes=read_control_value(*store.Get(),preview_status_key,max_status_bytes);
            return bytes ? std::optional{decode_status(*bytes)} : std::nullopt;
        };
        wait_for([&] { const auto s=status(); return s && s->state==PreviewState::ready; });
        EditSession editor(saved); LivePreview client(editor,store.Get());
        const auto initial_status=client.connect();
        const auto origin=read_control_value(*store.Get(),engine_identity_key,max_engine_identity_bytes);
        require(origin.has_value(),"APO did not identify its loaded engine");
        const auto identity=decode_engine_identity(*origin);
        wchar_t module_path[32768]{};require(GetModuleFileNameW(module.handle(),module_path,32768)>0,"Cannot read test module path");
        require(identity.runtime==initial_status.runtime&&identity.module_path==module_path,"Engine evidence identifies the wrong module or runtime");
        require(initial_status.sample_rate==rate && initial_status.channels==2,"bridge did not report actual format");
        require(initial_status.evidence==RuntimeEvidence::loaded && initial_status.active_fingerprint==processing_fingerprint(saved.configuration,rate,2),"APO startup did not identify the loaded bank before audio callbacks");
        const auto output_is=[&](double left,double right) {
            require(target.u32BufferFlags==BUFFER_VALID,"preview output invalid");
            for (UINT32 i=0; i<frames; ++i) {
                expect_near(output.data()[i*2],0.1*std::pow(10.0,left/20),"left preview output");
                expect_near(output.data()[i*2+1],0.1*std::pow(10.0,right/20),"right preview output");
            }
        };
        const auto settle=[&](double left,double right) {
            wait_for([&] {
                instance.process(source,target);
                const auto reply=client.poll();
                if (reply.feedback==PreviewFeedback::rejected) { throw std::runtime_error(reply.detail); }
                return reply.feedback==PreviewFeedback::applied;
            });
            output_is(left,right);
            require(editor.applied()==editor.snapshot(),"APO reply did not acknowledge the editor snapshot");
            wait_for([&] { const auto s=status(); return s && s->evidence==RuntimeEvidence::processed && s->active_fingerprint==processing_fingerprint(editor.working().configuration,rate,2); });
        };
        instance.process(source,target); output_is(-2,-3);
        wait_for([&] { const auto s=status(); return s && s->evidence==RuntimeEvidence::processed && s->active_fingerprint==processing_fingerprint(saved.configuration,rate,2); });
        editor.replace(gains(-8,-5)); client.submit();
        wait_for([&] { const auto s=status(); return s && s->state==PreviewState::queued; });
        require(client.poll().feedback==PreviewFeedback::waiting && !editor.applied(),"idle engine acknowledged without audio processing");
        instance.process(source,target);
        require(client.poll().feedback==PreviewFeedback::waiting,"partial audio crossfade acknowledged early");
        settle(-8,-5);
        require(read_profile_file(profile_path)==original && editor.dirty(),"preview overwrote saved profile");
        save_profile_settings(editor,*persisted.Get());
        editor.replace(gains(-12,-10)); client.submit(); settle(-12,-10);
        require(editor.undo(),"undo failed"); client.submit(); settle(-8,-5);
        require(editor.redo(),"redo failed"); client.submit(); settle(-12,-10);
        require(editor.revert(),"revert failed"); client.submit(); settle(-8,-5);
        if (rate==44100) {
            auto invalid_at_stream_rate=editor.working().configuration;
            invalid_at_stream_rate.layers[0].filters.push_back({FilterType::peak,23000,2,1});
            editor.replace(invalid_at_stream_rate); client.submit();
            wait_for([&] { return client.poll().feedback==PreviewFeedback::rejected; });
            require(editor.awaiting_preview(),"rejection acknowledged invalid settings");
            instance.process(source,target); output_is(-8,-5);
            require(editor.revert(),"revert rejected draft failed"); client.submit(); settle(-8,-5);
        }
        editor.replace(gains(-4,-6)); client.submit(); settle(-4,-6);
        instance.unlock();
        require(client.poll().feedback==PreviewFeedback::disconnected && !editor.applied(),"unlock retained active confirmation");
        instance.lock(in,out);
        wait_for([&] { const auto s=status(); return s && s->state==PreviewState::ready && s->runtime!=initial_status.runtime; });
        Sleep(250); // Let the new worker see the old request still in the volatile store.
        instance.process(source,target); output_is(-8,-5);
        require(client.poll().feedback==PreviewFeedback::disconnected && editor.dirty(),"restart retained old confirmation or discarded draft");
        client.connect();
        instance.process(source,target); output_is(-8,-5); // Connecting itself does not apply the draft.
        client.submit(); settle(-4,-6);
        require(editor.revert(),"final revert failed"); client.submit(); settle(-8,-5);
        require(read_saved_profile(*persisted.Get())==std::optional{editor.saved()},"saved state changed across preview/restart");
        require(read_profile_file(profile_path)==original,"settings save altered the legacy package file");
        editor.replace(gains(-9,-7)); save_profile_settings(editor,*persisted.Get());
        require(!editor.dirty() && editor.awaiting_preview(),"save falsely confirmed audio application");
        instance.process(source,target); output_is(-8,-5);
        instance.unlock(); instance.lock(in,out); instance.process(source,target); output_is(-9,-7);
        instance.unlock();
        // A master toggle saved while no runtime exists must take effect at
        // the first callback, with no editor/client present to submit it.
        auto bypassed=editor.saved();bypassed.configuration.bypass=true;
        write_saved_profile(*persisted.Get(),bypassed);
        instance.lock(in,out);instance.process(source,target);output_is(0,0);instance.unlock();
        bypassed.configuration.bypass=false;write_saved_profile(*persisted.Get(),bypassed);
        instance.lock(in,out);instance.process(source,target);output_is(-9,-7);instance.unlock();
        const auto neutral_on_restart=[&](bool expect_error=true) {
            const auto previous=status();
            instance.lock(in,out); instance.process(source,target); output_is(0,0);
            wait_for([&] { const auto s=status(); return s && (!previous || s->runtime!=previous->runtime); });
            require(status()->detail.empty()!=expect_error,"startup settings error was hidden or invented");
            wait_for([&] { const auto s=status(); return s && s->evidence==RuntimeEvidence::processed && s->active_fingerprint==processing_fingerprint({},rate,2); });
            instance.unlock();
        };
        write_control_value(*persisted.Get(),saved_profile_key,"broken saved settings"); neutral_on_restart();
        write_control_value(*persisted.Get(),saved_profile_key,serialize_profile({gains(-4,-5),48000,4})); neutral_on_restart();
        if (rate==44100) {
            Profile high{{{{"High",{},{{FilterType::peak,23000,2,1}}}}},48000,2};
            write_control_value(*persisted.Get(),saved_profile_key,serialize_profile(high)); neutral_on_restart();
        }
        PROPVARIANT empty{}; hr(persisted->SetValue(saved_profile_key,empty),S_OK,"clear saved fixture");
        neutral_on_restart(false);
        persisted->read_result=E_ACCESSDENIED; neutral_on_restart(); persisted->read_result=S_OK;
        input.guards(); output.guards();
    }
    hr(module.can_unload(),S_OK,"control worker and marshaled settings references released");
}

void test_device_recovery(const Module& module) {
    using namespace contour::eq;
    constexpr auto id=L"host-selected-device";
    const Profile saved{{{{"All",{},{},-3}}},48000,2};
    const auto persisted=Microsoft::WRL::Make<control_test::Store>();
    write_saved_profile(*persisted.Get(),saved);
    EditSession editor(saved);
    const auto enumerator=Microsoft::WRL::Make<control_test::Enumerator>(id,nullptr);
    EndpointSession session(editor,id,enumerator.Get());
    editor.replace({{{"Draft",{},{},-9}}}); const auto draft=editor.snapshot();
    struct Format { float rate; UINT32 channels; };
    for (const auto layout : {Format{48000,2},Format{44100,2},Format{48000,4},Format{96000,2}}) {
        const auto store=Microsoft::WRL::Make<control_test::Store>();
        const auto endpoint=Microsoft::WRL::Make<control_test::Endpoint>(store.Get(),persisted.Get());
        const auto devices=Microsoft::WRL::Make<control_test::Devices>(endpoint.Get());
        enumerator->endpoint(endpoint.Get()); enumerator->lookup_result=S_OK;
        enumerator->callback->OnDeviceAdded(id);
        Instance instance(module,test_post_clsid,false);
        APOInitSystemEffects init{}; init.APOInit={sizeof(init),test_post_clsid}; init.pDeviceCollection=devices.Get();
        hr(instance.apo->Initialize(sizeof(init),reinterpret_cast<BYTE*>(&init)),S_OK,"initialize returning endpoint APO");
        constexpr UINT32 frames=128;
        auto media=format(layout.channels,layout.rate); Buffer input(frames*layout.channels),output(frames*layout.channels); input.fill(0.1F);
        auto in=descriptor(media.Get(),input.data(),frames),out=descriptor(media.Get(),output.data(),frames);
        auto source=property(input.data(),frames,BUFFER_VALID),target=property(output.data(),0,BUFFER_INVALID);
        instance.lock(in,out);
        const auto deadline=GetTickCount64()+3000;
        while (!session.refresh().runtime && GetTickCount64()<deadline) { Sleep(1); }
        const auto current=session.refresh(); require(current.runtime.has_value(),"Returning APO never reported its format");
        require(current.runtime->sample_rate==layout.rate && current.runtime->channels==layout.channels,"Wrong rebuilt runtime format");
        instance.process(source,target);
        const auto initial=layout.channels==2 ? 0.1*std::pow(10.0,-3.0/20) : 0.1;
        for (UINT32 i=0; i<frames*layout.channels; ++i) { expect_near(output.data()[i],initial,"reconnect loaded saved settings without replaying draft"); }
        require(!read_control_value(*store.Get(),preview_request_key,max_request_bytes) && !editor.applied(),"Reconnect automatically published or confirmed draft");
        if (layout.channels==2) {
            require(current.state==EndpointState::ready,"Compatible rebuilt graph did not become ready");
            session.preview();
            const auto applied_deadline=GetTickCount64()+3000;
            while (session.refresh().preview!=PreviewFeedback::applied && GetTickCount64()<applied_deadline) { instance.process(source,target); Sleep(1); }
            require(editor.applied()==editor.snapshot(),"Returning graph never acknowledged explicit preview");
            instance.process(source,target);
            for (UINT32 i=0; i<frames*2; ++i) { expect_near(output.data()[i],0.1*std::pow(10.0,-9.0/20),"reconnected preview output"); }
        } else { require(current.state==EndpointState::incompatible,"Four-channel device accepted a stereo draft"); }
        instance.unlock(); enumerator->lookup_result=E_NOTFOUND; enumerator->callback->OnDeviceRemoved(id);
        require(session.refresh().state==EndpointState::unavailable && !editor.applied(),"Removal left the old APO confirmed");
        require(editor.snapshot()==draft && editor.saved()==saved && editor.can_undo(),"Device lifecycle lost editing state");
        input.guards(); output.guards();
    }
}

int run(const std::filesystem::path& path, bool component) {
    test_component = component;
    test_pre_clsid = component ? component_pre_mix_clsid : pre_mix_clsid;
    test_post_clsid = component ? component_post_mix_clsid : post_mix_clsid;
    default_db = component ? 0.0 : -6.0;
    const ComRuntime com;
    const Module module(path);
    std::cout << "Contour standalone APO host -- no device attachment or playback\n";
    test::Suite suite;
    suite.run("COM identity, factories, registration metadata, and lifetime", [&] { test_identity(module); });
    suite.run("engine-style COM aggregation, lifetime, and processing", [&] { test_aggregation(module); });
    suite.run("initialization, latency, and frame mapping", [&] { test_initialization(module); });
    suite.run("float formats, PCM suggestions, and mismatched formats", [&] { test_negotiation(module); });
    suite.run("lock validation, reset, unlock, and format changes", [&] { test_lifecycle(module); });
    suite.run("pre-mix pass-through, separate buffers", [&] { test_processing(module, test_pre_clsid, false); });
    suite.run("pre-mix pass-through, in-place", [&] { test_processing(module, test_pre_clsid, true); });
    suite.run("post-mix gain, separate buffers", [&] { test_processing(module, test_post_clsid, false); });
    suite.run("post-mix gain, in-place", [&] { test_processing(module, test_post_clsid, true); });
    suite.run("silence, invalid buffers, bounds, and channel isolation", [&] { test_flags(module); });
    suite.run("pre/post chain applies the expected gain once", [&] { test_chain(module); });
    suite.run(component ? "device-scoped identity ignores package-wide lab profiles" :
        "saved PEQ, actual stream rates, tails, live edits, rejection and bypass", [&] { test_live_profile(path); });
    suite.run("editor to actual APO through Windows properties, acknowledgement, rejection, save/revert, and restart", [&] { test_control_bridge(path); });
    suite.run("device controller across actual APO recreation, rate/channel changes, preserved drafts, and explicit preview", [&] { test_device_recovery(module); });
    suite.run("DLL unloads after all test instances", [&] { hr(module.can_unload(), S_OK, "final lifetime"); });
    return suite.finish();
}
} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--help") {
        std::cout << "Usage: contour-apo-host <path-to-contour-apo.dll> [--component]\n"
                     "Load the DLL directly and validate its COM/APO contracts and sample output.\n"
                     "Does not register effects, attach to devices, or play audio.\n";
        return 0;
    }
    if (argc != 2 && !(argc == 3 && std::wstring_view(argv[2]) == L"--component")) {
        std::cerr << "Expected a DLL path. Use contour-apo-host --help.\n";
        return 2;
    }
    try {
        return run(argv[1], argc == 3);
    } catch (const std::exception& error) {
        std::cerr << "Contour APO host: " << error.what() << '\n';
        return 1;
    }
}
