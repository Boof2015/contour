#include <windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <endpointvolume.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <contour/profile.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
constexpr double amplitude = 0.01; // -40 dBFS peak. Never modifies hardware/session volume.
constexpr std::array frequencies{997.0, 1499.0};
constexpr double tau = 2.0 * std::numbers::pi;

void require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}
void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        std::ostringstream message;
        message << operation << " failed (HRESULT 0x" << std::hex
                << static_cast<unsigned long>(result) << ")";
        throw std::runtime_error(message.str());
    }
}
struct Com {
    Com() { check(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "COM initialization"); }
    ~Com() { CoUninitialize(); }
};
struct Event {
    HANDLE value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Event() { require(value != nullptr, "CreateEvent failed"); }
    ~Event() { CloseHandle(value); }
};
struct Scheduling {
    DWORD index = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Audio", &index);
    ~Scheduling() { if (task) { AvRevertMmThreadCharacteristics(task); } }
};
struct RunningStream {
    IAudioClient* client;
    explicit RunningStream(IAudioClient* value) : client(value) { check(client->Start(), "start audio stream"); }
    ~RunningStream() { client->Stop(); }
};

struct ToneFit {
    double amplitude;
    double residual_rms;
    double snr_db;
    double peak;
};
ToneFit fit(const float* interleaved, std::size_t start, UINT32 rate, UINT32 channel) {
    // Exactly one second, so both integer-Hz tones occupy orthogonal DFT bins.
    // Quadrature fitting removes any dependence on capture latency/phase.
    double sine = 0, cosine = 0, energy = 0, mean = 0, peak = 0;
    for (UINT32 frame = 0; frame < rate; ++frame) {
        const double value = interleaved[(start + frame) * 2 + channel];
        require(std::isfinite(value), "Non-finite sample in capture");
        const double phase = tau * frequencies[channel] * frame / rate;
        sine += value * std::sin(phase);
        cosine += value * std::cos(phase);
        energy += value * value;
        mean += value;
        peak = std::max(peak, std::abs(value));
    }
    const double measured = 2.0 * std::hypot(sine, cosine) / rate;
    mean /= rate;
    const double residual = std::sqrt(std::max(0.0, energy / rate - measured * measured / 2 - mean * mean));
    const double snr = 20 * std::log10(std::max(measured / std::sqrt(2.0), 1.0e-20) /
                                      std::max(residual, 1.0e-20));
    return {measured, residual, snr, peak};
}

void self_test() {
    for (const UINT32 rate : {44100U, 48000U, 96000U}) {
        std::vector<float> samples(static_cast<std::size_t>(rate) * 2);
        const double gain = std::pow(10.0, -6.0 / 20.0);
        for (UINT32 frame = 0; frame < rate; ++frame) {
            for (UINT32 channel = 0; channel < 2; ++channel) {
                samples[frame * 2 + channel] = static_cast<float>(amplitude * gain *
                    std::sin(tau * frequencies[channel] * frame / rate + 0.37 + channel));
            }
        }
        for (UINT32 channel = 0; channel < 2; ++channel) {
            const auto measured = fit(samples.data(), 0, rate, channel);
            require(std::abs(20 * std::log10(measured.amplitude / amplitude) + 6) < 0.00001,
                    "Phase-independent gain measurement failed");
            require(measured.snr_db > 100, "Pure-tone residual incorrect");
        }
        std::fill(samples.begin(), samples.end(), 0.0F);
        require(fit(samples.data(), 0, rate, 0).amplitude == 0, "Silent capture detected as tone");
        for (UINT32 frame = 0; frame < rate; ++frame) {
            samples[frame * 2] = static_cast<float>(amplitude * std::sin(tau * frequencies[1] * frame / rate));
        }
        require(fit(samples.data(), 0, rate, 0).amplitude < 1.0e-9, "Wrong-channel tone accepted");
        for (UINT32 frame = 0; frame < rate; ++frame) {
            samples[frame * 2] += static_cast<float>(amplitude * std::sin(tau * frequencies[0] * frame / rate));
        }
        require(std::abs(fit(samples.data(), 0, rate, 0).snr_db) < 0.001,
                "Contaminating signal not detected");
    }
    std::cout << "PASS tone analysis: phase, gain, silence, channel separation, contamination at 3 sample rates\n";
}

std::string json_string(std::wstring_view text) {
    std::ostringstream out;
    out << '"';
    for (const wchar_t c : text) {
        if (c >= 32 && c < 127 && c != L'"' && c != L'\\') { out << static_cast<char>(c); }
        else { out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(c); }
    }
    out << '"';
    return out.str();
}

// Writes happen on a separate control thread while the same render/capture
// clients keep running. The audio pump only announces capture progress.
class ProfileUpdates {
public:
    ProfileUpdates(const std::filesystem::path& destination, const std::filesystem::path& update,
                   const std::filesystem::path& bypass) : destination_(destination) {
        const auto a = contour::eq::read_profile_file(update), b = contour::eq::read_profile_file(bypass);
        require(a.has_value() && b.has_value(), "Missing update profile");
        profiles_[0] = contour::eq::parse_profile(*a);
        profiles_[1] = contour::eq::parse_profile(*b);
        require(!profiles_[0].configuration.bypass && profiles_[1].configuration.bypass,
            "Expected an enabled update followed by bypass");
        require(contour::eq::read_profile_file(destination_).has_value(), "Installed profile does not exist");
    }
    ~ProfileUpdates() { stop(); }
    void start() {
        worker_ = std::thread([this] {
            try {
                unsigned applied = 0;
                while (WaitForSingleObject(stop_.value, 5) == WAIT_TIMEOUT) {
                    const auto requested = requested_.load();
                    while (applied < requested) {
                        contour::eq::save_profile_file(destination_,profiles_[applied++]);
                        applied_.store(applied);
                    }
                }
            } catch (...) { error_ = std::current_exception(); }
        });
    }
    void progress(std::size_t frames, UINT32 rate) {
        requested_.store(frames >= static_cast<std::size_t>(rate)*7 ? 2u : frames >= static_cast<std::size_t>(rate)*3 ? 1u : 0u);
    }
    void finish() {
        stop();
        if (error_) { std::rethrow_exception(error_); }
        require(applied_.load() == 2, "Profile updates did not finish during capture");
    }
private:
    void stop() { if (worker_.joinable()) { SetEvent(stop_.value); worker_.join(); } }
    std::filesystem::path destination_;
    std::array<contour::eq::Profile,2> profiles_;
    Event stop_;
    std::thread worker_;
    std::atomic<unsigned> requested_{0}, applied_{0};
    std::exception_ptr error_;
};

int measure(const wchar_t* endpoint_id, const std::filesystem::path& output_path, ProfileUpdates* updates = nullptr) {
    require(!std::filesystem::exists(output_path), "Output already exists; use a new measurement filename");
    const Com com;
    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator)), "create device enumerator");
    ComPtr<IMMDevice> device;
    check(enumerator->GetDevice(endpoint_id, &device), "find selected playback endpoint");
    ComPtr<IMMEndpoint> endpoint;
    check(device.As(&endpoint), "endpoint flow interface");
    EDataFlow flow{};
    check(endpoint->GetDataFlow(&flow), "endpoint flow");
    require(flow == eRender, "An explicit playback endpoint is required");
    ComPtr<IAudioEndpointVolume> volume;
    check(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(volume.GetAddressOf())), "read endpoint volume");
    float volume_before = 0, volume_after = 0;
    BOOL muted = FALSE;
    check(volume->GetMasterVolumeLevelScalar(&volume_before), "get endpoint level");
    check(volume->GetMute(&muted), "get endpoint mute");
    require(!muted && volume_before > 0, "Endpoint is muted or at zero volume");
    ComPtr<IAudioClient> render, capture;
    check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(render.GetAddressOf())), "activate render client");
    check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(capture.GetAddressOf())), "activate loopback client");
    WAVEFORMATEX* raw = nullptr;
    check(render->GetMixFormat(&raw), "get mix format");
    const std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> format(raw, CoTaskMemFree);
    bool is_float = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extended = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format.get());
        constexpr GUID float_format = {STATIC_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT};
        is_float = extended->SubFormat == float_format && extended->Samples.wValidBitsPerSample == 32;
    }
    require(is_float && format->nChannels == 2 && format->wBitsPerSample == 32 &&
        format->nBlockAlign == 8 && format->nSamplesPerSec >= 8000 && format->nSamplesPerSec <= 192000,
        "Live test currently requires stereo float32, 8-192 kHz");
    const UINT32 rate = format->nSamplesPerSec;
    GUID session{};
    check(CoCreateGuid(&session), "create isolated test session");
    const Event render_event, capture_event;
    check(render->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
        AUDCLNT_STREAMFLAGS_NOPERSIST, 1000000, 0, format.get(), &session), "initialize render");
    check(render->SetEventHandle(render_event.value), "set render event");
    check(capture->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK |
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 1000000, 0, format.get(), nullptr), "initialize loopback");
    check(capture->SetEventHandle(capture_event.value), "set capture event");
    ComPtr<ISimpleAudioVolume> session_volume;
    check(render->GetService(IID_PPV_ARGS(&session_volume)), "get session volume");
    float session_level = 0;
    check(session_volume->GetMasterVolume(&session_level), "get session level");
    check(session_volume->GetMute(&muted), "get session mute");
    require(!muted && session_level > 0, "Test session is muted or at zero volume");
    ComPtr<IAudioRenderClient> renderer;
    ComPtr<IAudioCaptureClient> recorder;
    check(render->GetService(IID_PPV_ARGS(&renderer)), "get render buffer interface");
    check(capture->GetService(IID_PPV_ARGS(&recorder)), "get capture buffer interface");
    UINT32 capacity = 0;
    check(render->GetBufferSize(&capacity), "get render capacity");

    // Precompute faded tones, followed by silence. Capture only
    // aggregate measurements; never write mixed system audio to disk.
    const UINT32 duration = updates ? 11 : 3;
    std::vector<float> signal(static_cast<std::size_t>(rate) * (duration+1) * 2, 0.0F);
    for (UINT32 frame = 0; frame < rate * duration; ++frame) {
        const double seconds = static_cast<double>(frame) / rate;
        const double envelope = std::clamp(std::min(seconds / 0.1, (duration - seconds) / 0.1), 0.0, 1.0);
        for (UINT32 channel = 0; channel < 2; ++channel) {
            signal[frame * 2 + channel] = static_cast<float>(amplitude * envelope *
                std::sin(tau * frequencies[channel] * seconds));
        }
    }
    std::vector<float> recorded;
    recorded.reserve(static_cast<std::size_t>(rate) * (duration+2) * 2);
    std::vector<std::size_t> discontinuities;
    discontinuities.reserve(32);
    std::size_t submitted = 0;
    const auto fill = [&](UINT32 frames) {
        if (frames == 0) { return; }
        BYTE* bytes = nullptr;
        check(renderer->GetBuffer(frames, &bytes), "get render buffer");
        auto* samples = reinterpret_cast<float*>(bytes);
        for (UINT32 frame = 0; frame < frames; ++frame) {
            const auto position = (submitted + frame) * 2;
            for (UINT32 channel = 0; channel < 2; ++channel) {
                samples[frame * 2 + channel] = position + channel < signal.size() ? signal[position + channel] : 0;
            }
        }
        check(renderer->ReleaseBuffer(frames, 0), "release render buffer");
        submitted += frames;
    };
    fill(capacity);
    {
        const Scheduling scheduling;
        const RunningStream capture_running(capture.Get());
        const RunningStream render_running(render.Get());
        if (updates) { updates->start(); }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration+3);
        const HANDLE events[] = {render_event.value, capture_event.value};
        while (recorded.size() / 2 < static_cast<std::size_t>(rate) * (duration*2+1) / 2) {
            require(std::chrono::steady_clock::now() < deadline, "Timed out collecting loopback audio");
            const DWORD wait = WaitForMultipleObjects(2, events, FALSE, 250);
            require(wait != WAIT_FAILED, "Waiting for audio events failed");
            UINT32 padding = 0;
            check(render->GetCurrentPadding(&padding), "get render padding");
            require(padding <= capacity, "Invalid render padding");
            fill(capacity - padding);
            UINT32 packet = 0;
            check(recorder->GetNextPacketSize(&packet), "get loopback packet size");
            while (packet != 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                check(recorder->GetBuffer(&data, &frames, &flags, nullptr, nullptr), "get loopback packet");
                const std::size_t first = recorded.size() / 2;
                if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) { discontinuities.push_back(first); }
                const auto* samples = reinterpret_cast<const float*>(data);
                if (recorded.size() + static_cast<std::size_t>(frames) * 2 > recorded.capacity()) {
                    recorder->ReleaseBuffer(frames);
                    throw std::runtime_error("Capture exceeded bounded storage");
                }
                for (std::size_t index = 0; index < static_cast<std::size_t>(frames) * 2; ++index) {
                    recorded.push_back(flags & AUDCLNT_BUFFERFLAGS_SILENT ? 0.0F : samples[index]);
                }
                check(recorder->ReleaseBuffer(frames), "release loopback packet");
                check(recorder->GetNextPacketSize(&packet), "get next loopback packet");
            }
            if (updates) { updates->progress(recorded.size()/2,rate); }
        }
    }
    if (updates) { updates->finish(); }
    check(volume->GetMasterVolumeLevelScalar(&volume_after), "get final endpoint level");
    check(volume->GetMute(&muted), "get final endpoint mute");
    float session_after = 0;
    check(session_volume->GetMasterVolume(&session_after), "get final session level");
    bool valid = !muted && volume_before == volume_after;
    check(session_volume->GetMute(&muted), "get final session mute");
    valid = valid && !muted && session_after == session_level;
    std::array<std::array<ToneFit,2>,3> phases{};
    const unsigned phase_count = updates ? 3u : 1u;
    for (unsigned phase=0; phase<phase_count; ++phase) {
        const std::size_t analysis_start = static_cast<std::size_t>(rate)*(1+phase*4);
        for (const auto position : discontinuities) {
            if (position >= analysis_start && position < analysis_start + rate) { valid = false; }
        }
        for (UINT32 channel = 0; channel < 2; ++channel) {
            auto& fitted = phases[phase][channel];
            fitted = fit(recorded.data(), analysis_start, rate, channel);
            valid = valid && fitted.amplitude > 1.0e-8 && fitted.snr_db > 30 && fitted.peak < 0.99;
        }
    }
    std::ofstream report(output_path, std::ios::binary);
    require(report.is_open(), "Cannot create measurement report");
    report << std::setprecision(12) << "{\n\"Schema\":1,\"Endpoint\":" << json_string(endpoint_id)
        << ",\"SampleRate\":" << rate << ",\"Valid\":" << (valid ? "true" : "false")
        << ",\"SourceAmplitude\":" << amplitude << ",\"EndpointVolumeBefore\":" << volume_before
        << ",\"EndpointVolumeAfter\":" << volume_after << ",\"SessionVolume\":" << session_level
        << ",\"CaptureFrames\":" << recorded.size()/2 << ",\"DiscontinuityCount\":" << discontinuities.size()
        << ",\"Channels\":";
    const auto write_channels = [&](const auto& fits) {
        report << '[';
        for (UINT32 channel = 0; channel < 2; ++channel) {
            if (channel != 0) { report << ','; }
            report << "{\"Frequency\":" << frequencies[channel] << ",\"Amplitude\":" << fits[channel].amplitude
                << ",\"ResidualRms\":" << fits[channel].residual_rms << ",\"SnrDb\":" << fits[channel].snr_db
                << ",\"Peak\":" << fits[channel].peak << '}';
            std::cout << "Channel " << channel << ": amplitude " << std::setprecision(9) << fits[channel].amplitude
                      << ", SNR " << fits[channel].snr_db << " dB\n";
        }
        report << ']';
    };
    write_channels(phases[0]);
    report << ",\"ContinuousStream\":" << (updates ? "true" : "false") << ",\"Phases\":[";
    for (unsigned phase=0; phase<phase_count; ++phase) {
        if (phase) { report << ','; }
        report << "{\"AnalysisStartSeconds\":" << 1+phase*4 << ",\"Channels\":";
        write_channels(phases[phase]); report << '}';
    }
    report << "]}\n";
    report.close();
    require(!report.fail(), "Failed writing measurement report");
    std::cout << (valid ? "Measurement valid\n" : "Measurement invalid: check competing audio, mute/volume, or discontinuities\n");
    return valid ? 0 : 3;
}
} // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        if (argc == 2 && std::wstring_view(argv[1]) == L"--self-test") { self_test(); return 0; }
        if (argc == 4 && std::wstring_view(argv[1]) == L"measure") { return measure(argv[2], argv[3]); }
        if (argc == 7 && std::wstring_view(argv[1]) == L"measure-updates") {
            ProfileUpdates updates(argv[4],argv[5],argv[6]);
            return measure(argv[2],argv[3],&updates);
        }
        std::cout << "Usage: contour-audio-test measure <playback-endpoint-ID> <new-report.json>\n"
                     "Plays three seconds of -40 dBFS stereo tones and measures WASAPI loopback.\n"
                     "Does not change volume. Saves aggregate statistics, not captured audio.\n"
                     "       contour-audio-test measure-updates <endpoint> <new-report.json> <installed.contour> <update.contour> <bypass.contour>\n"
                     "Keeps one stream open for eleven seconds; publishes the update at 3 s and bypass at 7 s.\n"
                     "       contour-audio-test --self-test (no playback)\n";
        return argc == 2 && std::wstring_view(argv[1]) == L"--help" ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "Contour audio test: " << error.what() << '\n';
        return 1;
    }
}
