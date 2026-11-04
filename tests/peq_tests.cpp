#include <contour/peq.h>
#include <contour/peq_import.h>
#include "test_support.h"

#include <algorithm>
#include <array>
#include <complex>
#include <cstdlib>
#include <limits>
#include <new>
#include <numbers>
#include <vector>

// Detect accidental heap allocation in the callback, including future changes.
namespace allocation_check { thread_local std::size_t count = 0; }
void* operator new(std::size_t bytes) {
    ++allocation_check::count;
    if (void* memory = std::malloc(bytes == 0 ? 1 : bytes)) { return memory; }
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

using namespace contour::eq;
using test::require;
namespace {
constexpr double pi = std::numbers::pi;
void near(double actual, double expected, double tolerance = 1e-7) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        throw std::runtime_error("Expected " + std::to_string(expected) + ", got " + std::to_string(actual));
    }
}
template<class Function> void rejects(Function function) {
    try { function(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Expected invalid_argument");
}
Configuration single(Filter filter) {
    Layer layer;
    layer.filters.push_back(filter);
    return {{layer}};
}

// Independent reference: analog transfer functions under the bilinear
// frequency mapping, rather than repeating the production coefficient code.
std::complex<double> reference(const Filter& f, double rate, double frequency) {
    const std::complex<double> s{0, std::tan(pi*frequency/rate) / std::tan(pi*f.frequency_hz/rate)};
    const double a = std::pow(10.0, f.gain_db/40);
    const auto denominator = s*s + s/f.q + 1.0;
    switch (f.type) {
    case FilterType::peak: return (s*s + s*a/f.q + 1.0) / (s*s + s/(a*f.q) + 1.0);
    case FilterType::low_shelf: return a*(s*s + s*std::sqrt(a)/f.q + a) / (a*s*s + s*std::sqrt(a)/f.q + 1.0);
    case FilterType::high_shelf: return a*(a*s*s + s*std::sqrt(a)/f.q + 1.0) / (s*s + s*std::sqrt(a)/f.q + a);
    case FilterType::low_pass: return 1.0/denominator;
    case FilterType::high_pass: return s*s/denominator;
    case FilterType::notch: return (s*s + 1.0)/denominator;
    case FilterType::all_pass: return (s*s - s/f.q + 1.0)/denominator;
    case FilterType::band_pass: return (s/f.q)/denominator;
    default: throw std::runtime_error("Invalid reference type");
    }
}

void test_response() {
    for (const double rate : {44100.0, 48000.0, 96000.0, 192000.0}) {
        for (const auto type : {FilterType::peak, FilterType::low_shelf, FilterType::high_shelf,
             FilterType::low_pass, FilterType::high_pass, FilterType::notch, FilterType::all_pass, FilterType::band_pass}) {
            for (const double frequency : {40.0, 1500.0, rate*0.4}) {
                Filter f{type, frequency, 0, 1.3};
                if (type == FilterType::peak || type == FilterType::low_shelf || type == FilterType::high_shelf) { f.gain_db = -9; }
                ParametricEq eq;
                eq.configure(single(f), rate, 1);
                for (const double probe : {10.0, 80.0, 1000.0, rate*0.31, rate*0.499}) {
                    const auto actual = eq.response(0, probe), expected = reference(f, rate, probe);
                    near(std::abs(actual-expected), 0, 2e-8);
                }
            }
        }
    }
    ParametricEq eq;
    eq.configure(single({FilterType::peak, 1000, 6, 4}), 48000, 1);
    near(eq.response_db(0, 1000), 6);
    near(eq.response_db(0, 0), 0);
    near(eq.response_db(0, 24000), 0);
    eq.configure(single({FilterType::low_shelf, 1000, 12, butterworth_q}), 48000, 1);
    near(eq.response_db(0, 0), 12);
    near(eq.response_db(0, 1000), 6);
    near(eq.response_db(0, 24000), 0);
    eq.configure(single({FilterType::low_pass, 1000, 0, butterworth_q}), 48000, 1);
    near(eq.response_db(0, 1000), -3.01029995664);
}

void test_sample_response() {
    constexpr std::size_t rate = 48000, frames = rate*2;
    for (const auto type : {FilterType::peak, FilterType::low_shelf, FilterType::high_shelf,
         FilterType::low_pass, FilterType::high_pass, FilterType::notch, FilterType::all_pass, FilterType::band_pass}) {
        Filter filter{type, 750, 0, 1.2};
        if (type == FilterType::peak || type == FilterType::low_shelf || type == FilterType::high_shelf) { filter.gain_db = 8; }
        ParametricEq eq;
        eq.configure(single(filter), rate, 2);
        std::vector<float> samples(frames*2);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            samples[frame*2] = static_cast<float>(0.1*std::cos(2*pi*997*static_cast<double>(frame)/rate));
        }
        eq.process(samples.data(), samples.data(), frames);
        std::complex<double> fit{};
        for (std::size_t frame = rate; frame < frames; ++frame) {
            fit += static_cast<double>(samples[frame*2]) * std::polar(1.0, -2*pi*997*static_cast<double>(frame)/rate);
            require(samples[frame*2+1] == 0, "shared filters leaked history to silent channel");
        }
        fit *= 2.0/(rate*0.1);
        near(std::abs(fit - reference(filter, rate, 997)), 0, 2e-7);
    }
}

void test_routing() {
    Layer first{"First pair", ChannelSelection::selected(0b0011), {{FilterType::peak, 1000, -6, 2}}};
    Layer second{"Second pair", ChannelSelection::selected(0b1100), {{FilterType::peak, 1000, -3, 2}}};
    Layer all{"All", {}, {{FilterType::peak, 1000, 2, 2}}};
    Layer overlap{"Overlap", ChannelSelection::selected(0b0110), {}, -1};
    ParametricEq eq;
    eq.configure({{first, second, all, overlap}}, 48000, 4);
    const std::array expected{-4.0, -5.0, -2.0, -1.0};
    for (std::size_t c = 0; c < 4; ++c) { near(eq.response_db(c, 1000), expected[c]); }
    constexpr std::size_t frames = 96000;
    std::vector<float> samples(frames*4);
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::size_t c = 0; c < 4; ++c) { samples[f*4+c] = static_cast<float>(0.1*std::sin(2*pi*1000*static_cast<double>(f)/48000)); }
    }
    eq.process(samples.data(), samples.data(), frames);
    for (std::size_t c = 0; c < 4; ++c) {
        double energy = 0;
        for (std::size_t f = 48000; f < frames; ++f) { energy += static_cast<double>(samples[f*4+c])*samples[f*4+c]; }
        near(10*std::log10(energy/(48000*0.005)), expected[c], 2e-6);
    }
    // Channel 32 exercises the top mask bit without a 32-bit shift overflow.
    first.channels = ChannelSelection::selected(0x80000000u);
    eq.configure({{first}}, 48000, 32);
    near(eq.response_db(0, 1000), 0);
    near(eq.response_db(31, 1000), -6);
    eq.configure({{all}}, 48000, 1);
    near(eq.response_db(0, 1000), 2);
}

void test_history() {
    Configuration config = single({FilterType::peak, 60, 12, 20});
    config.layers[0].channels = ChannelSelection::selected(1);
    ParametricEq whole, blocks, disjoint;
    for (auto* eq : {&whole, &blocks, &disjoint}) { eq->configure(config, 48000, 2); }
    std::vector<float> input(8192*2, 0), output(input.size()+2, 123);
    input[0] = 0.25F;
    for (std::size_t f = 0; f < 8192; ++f) { input[f*2+1] = static_cast<float>(static_cast<int>(f%17)-8)/32; }
    auto a = input, b = input;
    const auto allocation_count = allocation_check::count;
    whole.process(a.data(), a.data(), 8192);
    disjoint.process(input.data(), output.data()+1, 8192);
    require(allocation_check::count == allocation_count, "audio processing allocated heap memory");
    std::size_t position = 0;
    while (position < 8192) {
        const auto count = std::min<std::size_t>(1+(position*37)%257, 8192-position);
        blocks.process(b.data()+position*2, b.data()+position*2, count);
        position += count;
    }
    require(a == b, "filter history depends on block partition");
    require(std::equal(a.begin(), a.end(), output.begin()+1), "in-place differs from disjoint");
    require(output.front() == 123 && output.back() == 123, "output guard overwritten");
    require(std::abs(a[100]) > 1e-8, "zero input failed to emit IIR tail");
    for (std::size_t f = 0; f < 8192; ++f) { require(a[f*2+1] == input[f*2+1], "untargeted channel changed"); }
    whole.reset();
    std::array<float, 2> silence{};
    whole.process(silence.data(), silence.data(), 1);
    require(silence == std::array<float,2>{}, "reset left stale history");
    whole.process(nullptr, nullptr, 0);
    std::array<float, 4> poisoned{std::numeric_limits<float>::quiet_NaN(), 0, std::numeric_limits<float>::infinity(), 0};
    whole.process(poisoned.data(), poisoned.data(), 2);
    require(std::all_of(poisoned.begin(), poisoned.end(), [](float f) { return f == 0; }), "invalid input poisoned state");
}

void test_validation() {
    const auto good = single({FilterType::peak, 1000, -6, 2});
    ParametricEq eq, reference_eq;
    eq.configure(good, 48000, 2); reference_eq.configure(good, 48000, 2);
    std::array<float, 2> impulse{1, 0}, copy = impulse;
    eq.process(impulse.data(), impulse.data(), 1); reference_eq.process(copy.data(), copy.data(), 1);
    rejects([&] { eq.configure(good, 0, 2); });
    rejects([&] { eq.configure(good, 48000, 0); });
    rejects([&] { eq.configure(good, 48000, 33); });
    for (const auto selection : {ChannelSelection::selected(0), ChannelSelection::selected(4), ChannelSelection{true, 1}}) {
        auto bad = good; bad.layers[0].channels = selection;
        rejects([&] { eq.configure(bad, 48000, 2); });
    }
    for (const double f : {0.0, 24000.0, 30000.0, std::numeric_limits<double>::quiet_NaN()}) {
        auto bad = good; bad.layers[0].filters[0].frequency_hz = f;
        rejects([&] { eq.configure(bad, 48000, 2); });
    }
    for (const double q : {0.0, -1.0, 1001.0, std::numeric_limits<double>::infinity()}) {
        auto bad = good; bad.layers[0].filters[0].q = q;
        rejects([&] { eq.configure(bad, 48000, 2); });
    }
    auto bad = good;
    bad.layers.resize(max_layers+1);
    rejects([&] { eq.configure(bad, 48000, 2); });
    bad = good; bad.layers[0].filters.resize(max_filters_per_layer+1);
    rejects([&] { eq.configure(bad, 48000, 2); });
    bad = good; bad.layers[0].filters.assign(64, good.layers[0].filters[0]); bad.layers.assign(5, bad.layers[0]);
    rejects([&] { eq.configure(bad, 48000, 2); });
    std::array<float, 128> tail{}, expected{};
    eq.process(tail.data(), tail.data(), 64); reference_eq.process(expected.data(), expected.data(), 64);
    require(tail == expected, "failed preparation changed coefficients or state");
    eq.configure(good, 96000, 2);
    near(eq.response_db(0, 1000), -6);
    for (const double gain : {-121.0, 37.0, std::numeric_limits<double>::quiet_NaN()}) {
        bad = good; bad.layers[0].filters[0].gain_db = gain;
        rejects([&] { eq.configure(bad, 48000, 2); });
    }
    bad = good; bad.layers[0].filters[0].type = static_cast<FilterType>(99);
    rejects([&] { eq.configure(bad, 48000, 2); });
}

void test_extremes() {
    std::size_t checked = 0, precision_rejections = 0;
    for (const double rate : {8000.0, 48000.0, 384000.0}) {
        for (const double frequency : {1.0, rate*0.499}) {
            for (const double gain : {-120.0, 36.0}) {
                for (const double q : {0.05, 1000.0}) {
                    ParametricEq eq;
                    try { eq.configure(single({FilterType::peak, frequency, gain, q}), rate, 1); }
                    catch (const std::invalid_argument& error) {
                        require(std::string_view(error.what()).find("center accuracy") != std::string_view::npos,
                                "unexpected boundary rejection");
                        ++precision_rejections;
                        continue;
                    }
                    ++checked;
                    std::array<float, 4096> samples{};
                    samples[0] = 0.001F;
                    eq.process(samples.data(), samples.data(), samples.size());
                    require(std::all_of(samples.begin(), samples.end(), [](float f) { return std::isfinite(f); }),
                            "nonfinite output at supported parameter extremes");
                    try { near(eq.response_db(0, frequency), gain, 0.02); }
                    catch (const std::exception& error) {
                        throw std::runtime_error("Rate " + std::to_string(rate) + ", Fc " + std::to_string(frequency) +
                            ", Q " + std::to_string(q) + ": " + error.what());
                    }
                }
            }
        }
    }
    require(checked >= 20 && precision_rejections > 0, "boundary matrix did not cover accuracy rejection");
    ParametricEq eq;
    rejects([&] { eq.configure(single({FilterType::peak, 1, -120, 1000}), 384000, 1); });
}

void test_bypass_and_headroom() {
    Layer layer{"", {}, {{FilterType::peak, 1023.456, 9, 100}}};
    ParametricEq eq;
    eq.configure({{layer}}, 48000, 2);
    const auto estimate = eq.estimate_headroom(0);
    near(estimate.peak_db, 9, 1e-6); // Landmark catches a narrow peak between grid points.
    near(estimate.suggested_attenuation_db, 9, 1e-6);
    Layer inverse = layer; inverse.filters[0].gain_db = -9;
    eq.configure({{layer, inverse}}, 48000, 2);
    near(eq.estimate_headroom(0).suggested_attenuation_db, 0, 1e-7);
    inverse.enabled = false;
    eq.configure({{layer, inverse}}, 48000, 2);
    near(eq.response_db(0, 1023.456), 9);
    layer.filters[0].enabled = false; layer.gain_db = -2;
    eq.configure({{layer, inverse}}, 48000, 2);
    near(eq.response_db(0, 1000), -2);
    near(eq.estimate_headroom(0).suggested_attenuation_db, 0);
    eq.configure({{layer, inverse}, true}, 48000, 2);
    require(eq.section_count(0) == 0, "global bypass retains active sections");
    std::array<float, 4> data{0.4F, -0.3F, 0.7F, 0};
    const auto original = data;
    eq.process(data.data(), data.data(), 2);
    require(data == original, "global bypass is not unity");
}

void test_import() {
    const auto imported = import_peq_text("\xEF\xBB\xBF" "Filter Settings file\r\nRoom EQ V5.40\r\n"
        "Dated: example\r\nEqualiser: Generic\r\nExample measurement\r\nTarget level: 75 dB\r\n"
        "Preamp: -3dB\r\nFilter 1: ON PK Fc 60Hz Gain -6.0dB Q 4.5\r\n"
        "Filter 2: OFF PK Fc 1234 Hz Gain +2 dB Q 1\r\nFilter 3: ON None\r\n"
        "Filter 4: ON LS Fc 100 Hz Gain 4 dB\r\nFilter 5: ON HSQ Fc 8000 Hz Gain -2dB Q .8 # comment\r\n");
    require(imported.layer.has_value(), "REW import rejected");
    require(imported.issues.size() == 1 && !imported.issues[0].error, "description warning missing");
    require(imported.layer->filters.size() == 4 && !imported.layer->filters[1].enabled, "OFF/None handling");
    near(imported.layer->gain_db, -3);
    Layer manual;
    manual.gain_db = -3;
    manual.filters = {{FilterType::peak,60,-6,4.5}, {FilterType::peak,1234,2,1,false},
        {FilterType::low_shelf,100,4,shelf_q_from_slope(4,0.9)}, {FilterType::high_shelf,8000,-2,0.8}};
    ParametricEq a,b;
    a.configure({{*imported.layer}},48000,4); b.configure({{manual}},48000,4);
    std::vector<float> signal(8192*4, 0), reference_signal;
    signal[0]=0.2F; signal[17]=0.3F; reference_signal=signal;
    a.process(signal.data(),signal.data(),8192); b.process(reference_signal.data(),reference_signal.data(),8192);
    require(signal == reference_signal, "imported filters differ from manually authored filters");
    auto editable = *imported.layer;
    editable.channels = ChannelSelection::selected(0b1010);
    editable.filters[0].gain_db = 3;
    a.configure({{editable}},48000,4);
    near(a.response_db(0,60),0);
    require(std::abs(a.response_db(1,60)) > 0.1, "import is not ordinary editable layer data");
    const auto other = import_peq_text("Filter: ON LP Fc 1000Hz\nFilter: ON HPQ Fc 20Hz Q 0.5\n"
        "Filter: ON NO Fc 60Hz Q 30\nFilter: ON AP Fc 300Hz Q 1\nFilter: ON BP Fc 200Hz Q 2\n"
        "Filter: ON LSC Fc 40Hz Gain 4dB Q .7\n");
    require(other.layer && other.layer->filters.size() == 6, "supported type import");
}

void test_import_errors() {
    const std::string good = "Filter 1: ON PK Fc 60Hz Gain -6dB Q 2\n";
    for (const std::string bad : {"Filter 2: ON PK Fc 60 Hz Gain NaNdB Q 2", "Filter 2: ON PK Fc 60Hz Gain 2dB",
        "Filter 2: ON PK Fc 60Hz Gain 2dB Q 0", "Filter 2: OFF FUTURE Fc 60Hz Gain 2dB Q 1",
        "Filter 2: ON LS 12dB Fc 100Hz Gain 4dB", "Filter 2: ON PK Fc 60Hz Gain 2dB BW Oct 1",
        "Filter 2: ON HP Fc 60Hz Gain 2dB Q 1", "Filter 1: ON None", "Channel: L", "Include: other.txt",
        "GraphicEQ: 20 0; 100 3", "Equaliser: FBQ2496", "Preamp: -1dB", "random unrecognised text"}) {
        const auto result = import_peq_text(good+bad);
        require(!result.layer && !result.issues.empty(), "bad import retained partial layer");
        require(result.issues.back().line == 2 && result.issues.back().error, "missing error line number");
    }
    require(!import_peq_text("# empty").layer, "empty import accepted");
    require(!import_peq_text("\xFF\xFE").layer, "UTF16 accepted as UTF8");
    require(!import_peq_text(std::string(1024*1024+1,' ')).layer, "oversize input accepted");
    const auto gain = import_peq_text("Preamp: -3 dB");
    require(gain.layer && gain.layer->filters.empty(), "gain-only layer rejected");
}
} // namespace

int main() {
    test::Suite suite;
    suite.run("eight filter types against independent analog-transfer reference", test_response);
    suite.run("actual samples match complex response; shared channels have independent history", test_sample_response);
    suite.run("two pairs, All, overlapping targets, and channel 32", test_routing);
    suite.run("block continuity, in-place, bounds, silence tails, reset, invalid samples", test_history);
    suite.run("invalid configurations preserve previous coefficients and history", test_validation);
    suite.run("parameter boundaries stay finite; imprecise extreme filters are rejected", test_extremes);
    suite.run("filter/layer/global bypass and combined headroom estimate", test_bypass_and_headroom);
    suite.run("REW import equals editable manual PEQ sample-for-sample", test_import);
    suite.run("unsupported/malformed imports fail atomically with line numbers", test_import_errors);
    return suite.finish();
}
