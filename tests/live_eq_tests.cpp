#include <contour/live_eq.h>
#include <contour/profile.h>
#include "test_support.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <thread>

using namespace contour::eq;
using test::require;
namespace {
Configuration gain(double db) { Layer layer; layer.gain_db = db; return {{layer}}; }
void close_to(double a, double b, double tolerance = 1e-6) { require(std::abs(a-b) < tolerance, "numeric mismatch"); }
void serialization() {
    Profile profile{{{{"Quotes \" and UTF-8 \xC3\xA9", ChannelSelection::selected(5),
        {{FilterType::peak, 123.4, -5.5, 8.2, false}, {FilterType::low_shelf, 80, 3, 0.7}}, -2, false}}}, 96000, 4};
    profile.configuration.bypass = true;
    const auto encoded = serialize_profile(profile);
    const auto decoded = parse_profile(encoded);
    require(serialize_profile(decoded) == encoded, "profile roundtrip lost settings");
    for (const std::string text : {"", "CONTOUR 2 48000 2\nBypass 0\nEnd\n", "CONTOUR 1 48000 2\nBypass 2\nEnd\n",
        "CONTOUR 1 48000 2\nBypass 0\nEnd\nInclude foo\n", "CONTOUR 1 48000 2\nBypass 0\nLayer \"x\" 1 0 4 0\nEndLayer\nEnd\n",
        "CONTOUR 1 48000 2\nBypass 0\nLayer \"x\" 1 1 0 0\nFilter 0 1 1000 0 0\nEndLayer\nEnd\n",
        "CONTOUR 1 48000 32\nBypass 0\nLayer \"x\" 1 0 -1 0\nEndLayer\nEnd\n",
        "CONTOUR 1 48000 -2\nBypass 0\nEnd\n", "CONTOUR 1 48000 2\nBypass -0\nEnd\n"}) {
        bool rejected = false;
        try { (void)parse_profile(text); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "malformed profile accepted");
    }
}
void persistence(const std::filesystem::path& directory) {
    const auto path = directory/L"atomic.contour";
    Profile a{gain(-3),48000,2}, b{gain(-9),48000,2};
    save_profile_file(path,a);
    std::atomic<bool> done{false}, valid{true};
    std::thread reader([&] {
        try { while (!done.load()) {
            const auto text = read_profile_file(path);
            require(text.has_value(), "missing atomic profile");
            const auto p = parse_profile(*text);
            const auto db = p.configuration.layers[0].gain_db;
            require(db == -3 || db == -9, "partial profile observed");
        } } catch (...) { valid = false; }
    });
    try { for (int i=0; i<30; ++i) { save_profile_file(path, i%2 ? a : b); } }
    catch (...) { done = true; reader.join(); throw; }
    done = true; reader.join(); require(valid, "atomic publication reader failed");
    const auto before = read_profile_file(path);
    a.channels = 0;
    try { save_profile_file(path,a); } catch (const std::invalid_argument&) {}
    require(read_profile_file(path) == before, "invalid save changed existing file");
    std::filesystem::remove(path);
}
void resident() {
    Configuration configuration{{{"One", ChannelSelection::selected(1), {{FilterType::peak, 80, 6, 12}}},
        {"All", {}, {{FilterType::high_shelf,8000,-3,0.7}}, -2}}};
    ParametricEq ordinary; ordinary.configure(configuration,48000,2);
    SmoothEq live; live.initialize(ResidentBank::prepare(configuration,48000,2,1),960);
    std::vector<float> source(8192*2), expected(source.size()), actual(source.size());
    source[0] = 0.3F; source[17] = 0.2F;
    ordinary.process(source.data(),expected.data(),8192);
    live.process(source.data(),actual.data(),8192);
    for (std::size_t i=0; i<actual.size(); ++i) { close_to(actual[i],expected[i],2e-7); }
    std::array<float,32> tail{};
    require(live.process(nullptr,tail.data(),16), "silent input lost IIR tail");
    live.reset();
    require(!live.process(nullptr,tail.data(),16), "reset left tail history");
}
void smoothing() {
    SmoothEq live; live.initialize(ResidentBank::prepare(gain(0),48000,2,1),960);
    require(live.publish(ResidentBank::prepare(gain(-12),48000,2,2)), "publish failed");
    require(live.publish(ResidentBank::prepare(gain(-6),48000,2,3)), "coalesce failed");
    std::vector<float> signal(1000*2,1);
    live.process(signal.data(),signal.data(),127);
    live.process(signal.data()+254,signal.data()+254,873);
    const double target = std::pow(10.0,-6.0/20);
    for (std::size_t f=0; f<1000; ++f) {
        const double t = static_cast<double>(std::min<std::size_t>(f,959))/959;
        close_to(signal[f*2],(1-t)+t*target,1e-7);
        require(signal[f*2] == signal[f*2+1], "fade differs between channels");
    }
    require(live.applied_revision() == 3, "wrong applied revision");
    require(!live.publish(ResidentBank::prepare(gain(0),48000,1,4)), "mismatched bank layout accepted");
    live.reclaim();
    Configuration bypass = gain(12); bypass.bypass = true;
    require(live.publish(ResidentBank::prepare(bypass,48000,2,5)), "bypass publish failed");
    signal.assign(1000*2,0.5F); live.process(signal.data(),signal.data(),1000);
    close_to(signal.back(),0.5);
}
void concurrency() {
    SmoothEq live; live.initialize(ResidentBank::prepare(gain(0),48000,2,1),32);
    std::atomic<bool> done{false}, valid{true};
    std::thread audio([&] {
        std::array<float,128> input{}, output{}; input.fill(0.1F);
        while (!done.load()) {
            live.process(input.data(),output.data(),64);
            for (const auto value : output) { if (!std::isfinite(value) || value < 0.01F || value > 0.101F) { valid = false; } }
        }
    });
    try {
        for (std::uint64_t revision=2; revision<=300; ++revision) {
            require(live.publish(ResidentBank::prepare(gain(-static_cast<double>(revision%12)),48000,2,revision)), "publish failed");
            std::this_thread::yield();
        }
        const auto deadline = GetTickCount64()+2000;
        while (live.applied_revision() != 300 && GetTickCount64() < deadline) { live.reclaim(); Sleep(1); }
        require(live.applied_revision() == 300, "latest profile did not converge");
    } catch (...) { done = true; audio.join(); throw; }
    done = true; audio.join(); require(valid, "invalid audio during concurrent edits");
}
} // namespace
int wmain(int argc,wchar_t* argv[]) {
    if (argc != 2) { return 2; }
    const auto directory = std::filesystem::path(argv[1]);
    std::filesystem::create_directories(directory);
    test::Suite suite;
    suite.run("versioned profile roundtrip and strict rejection",serialization);
    suite.run("atomic persistence while a concurrent reader loads settings",[&] { persistence(directory); });
    suite.run("resident runtime matches PEQ core, preserves silence tails and resets",resident);
    suite.run("20 ms crossfade, in-place buffers, coalescing, layout checks and bypass",smoothing);
    suite.run("concurrent profile publication and audio processing converge",concurrency);
    return suite.finish();
}
