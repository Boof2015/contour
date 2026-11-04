#include <contour/peq_import.h>
#include <contour/profile.h>

#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace {
using namespace contour::eq;
std::string ascii(std::wstring_view value) {
    std::string result;
    for (const auto c : value) {
        if (c > 127) { throw std::invalid_argument("Use ASCII numbers and channel selectors"); }
        result.push_back(static_cast<char>(c));
    }
    return result;
}
unsigned integer(std::string_view value) {
    unsigned parsed = 0;
    const auto result = std::from_chars(value.data(), value.data()+value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data()+value.size()) { throw std::invalid_argument("Expected an unsigned integer"); }
    return parsed;
}
ChannelSelection channels(std::string_view value, unsigned count) {
    if (value == "all" || value == "All" || value == "ALL") { return {}; }
    std::uint32_t mask = 0;
    while (!value.empty()) {
        const auto comma = value.find(',');
        const auto channel = integer(value.substr(0, comma));
        if (channel < 1 || channel > count) { throw std::invalid_argument("Selected channel is outside the layout"); }
        const auto bit = std::uint32_t{1} << (channel-1);
        if ((mask & bit) != 0) { throw std::invalid_argument("Duplicate channel in selection"); }
        mask |= bit;
        if (comma == value.npos) { break; }
        value.remove_prefix(comma+1);
        if (value.empty()) { throw std::invalid_argument("Trailing comma in channel selection"); }
    }
    if (mask == 0) { throw std::invalid_argument("Empty channel selection"); }
    return ChannelSelection::selected(mask);
}
std::string read(const std::filesystem::path& path) {
    if (std::filesystem::file_size(path) > 1024*1024) { throw std::invalid_argument("PEQ input exceeds 1 MiB"); }
    std::ifstream input(path, std::ios::binary);
    if (!input) { throw std::runtime_error("Cannot open PEQ input file"); }
    std::string result;
    char buffer[4096];
    while (input.read(buffer, sizeof(buffer)) || input.gcount()) {
        result.append(buffer, static_cast<std::size_t>(input.gcount()));
        if (result.size() > 1024*1024) { throw std::invalid_argument("PEQ input exceeds 1 MiB"); }
    }
    if (!input.eof()) { throw std::runtime_error("Failed reading PEQ input file"); }
    return result;
}
void string(std::string_view value) {
    std::cout << '"';
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') { std::cout << '\\' << c; }
        else if (c < 32) { std::cout << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(c) << std::dec; }
        else { std::cout << c; }
    }
    std::cout << '"';
}
void report(const Configuration& configuration, const ParametricEq& eq) {
    std::cout << std::setprecision(12) << "{\n  \"Schema\":1,\n  \"SampleRate\":" << eq.sample_rate()
        << ",\n  \"ChannelCount\":" << eq.channel_count()
        << ",\n  \"Bypass\":" << (configuration.bypass ? "true" : "false") << ",\n  \"Layers\":[\n";
    bool first = true;
    for (const auto& layer : configuration.layers) {
        if (!first) { std::cout << ",\n"; } first = false;
        std::cout << "    {\"Name\":"; string(layer.name);
        std::cout << ",\"Enabled\":" << (layer.enabled ? "true" : "false")
            << ",\"AllChannels\":" << (layer.channels.all ? "true" : "false")
            << ",\"ChannelMask\":" << layer.channels.mask << ",\"GainDb\":" << layer.gain_db << ",\"Filters\":[";
        bool first_filter = true;
        for (const auto& f : layer.filters) {
            if (!first_filter) { std::cout << ','; } first_filter = false;
            std::cout << "{\"Type\":"; string(filter_type_name(f.type));
            std::cout << ",\"FrequencyHz\":" << f.frequency_hz << ",\"GainDb\":" << f.gain_db
                << ",\"Q\":" << f.q << ",\"Enabled\":" << (f.enabled ? "true" : "false") << '}';
        }
        std::cout << "]}";
    }
    std::cout << "\n  ],\n  \"Channels\":[\n";
    for (std::size_t channel = 0; channel < eq.channel_count(); ++channel) {
        if (channel) { std::cout << ",\n"; }
        const auto headroom = eq.estimate_headroom(channel);
        std::cout << "    {\"Channel\":" << channel+1 << ",\"ActiveSections\":" << eq.section_count(channel)
            << ",\"EstimatedPeakDb\":" << headroom.peak_db << ",\"EstimatedPeakHz\":" << headroom.peak_frequency_hz
            << ",\"SuggestedAttenuationDb\":" << headroom.suggested_attenuation_db << ",\"Response\":[";
        bool first_point = true;
        for (const double frequency : {0.0,20.0,40.0,63.0,125.0,250.0,500.0,1000.0,2000.0,4000.0,8000.0,16000.0,eq.sample_rate()/2}) {
            if (frequency > eq.sample_rate()/2) { continue; }
            if (!first_point) { std::cout << ','; } first_point = false;
            std::cout << "{\"Hz\":" << frequency << ",\"Db\":" << eq.response_db(channel, frequency) << '}';
        }
        std::cout << "]}";
    }
    std::cout << "\n  ],\n  \"HeadroomIsEstimate\":true,\n  \"AutomaticGainApplied\":false\n}\n";
}
} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--help") {
        std::cout << "Usage: contour-eq-inspect <sample-rate> <channel-count> <all|1,2,...> <peq.txt> [<selection> <peq.txt> ...]\n"
            "Each selection/file pair adds one PEQ layer in order. Channel numbers start at 1.\n"
            "Prints normalized layers and each channel's combined response/headroom estimate as JSON.\n"
            "       --save <profile.contour> <rate> <channels> [<selection> <peq.txt> ...]\n"
            "       --profile <profile.contour>\n"
            "       --apply <source.contour> <destination.contour>\n"
            "       --bypass <on|off> <source.contour> <destination.contour>\n"
            "Save/apply/bypass atomically write the selected destination; an installed APO can watch it.\n"
            "Inspection is read-only. No command attaches an APO or plays sound.\n";
        return 0;
    }
    try {
        const std::wstring_view mode = argc > 1 ? argv[1] : L"";
        if (mode == L"--profile" || mode == L"--apply" || mode == L"--bypass") {
            const int expected = mode == L"--profile" ? 3 : mode == L"--apply" ? 4 : 5;
            if (argc != expected) { throw std::invalid_argument("Incorrect profile arguments; see --help"); }
            const auto text = read_profile_file(argv[mode == L"--bypass" ? 3 : 2]);
            if (!text) { throw std::invalid_argument("Source profile does not exist"); }
            auto profile = parse_profile(*text);
            if (mode == L"--bypass") {
                const std::wstring_view state = argv[2];
                if (state != L"on" && state != L"off") { throw std::invalid_argument("Bypass must be on or off"); }
                profile.configuration.bypass = state == L"on";
            }
            if (mode != L"--profile") { save_profile_file(argv[argc-1], profile); }
            ParametricEq eq;
            eq.configure(profile.configuration, profile.validation_sample_rate, profile.channels);
            report(profile.configuration, eq);
            return 0;
        }
        const bool save = mode == L"--save";
        const int first = save ? 3 : 1;
        if (argc < first+2 || (argc-first-2)%2 != 0) { throw std::invalid_argument("Expected rate, channel count and selection/file pairs. See --help"); }
        const auto rate = integer(ascii(argv[first])), count = integer(ascii(argv[first+1]));
        if (count < 1 || count > contour::dsp::max_channels) { throw std::invalid_argument("Channel count must be 1-32"); }
        if ((argc-first-2)/2 > static_cast<int>(max_layers)) { throw std::invalid_argument("Too many layers (maximum 32)"); }
        Configuration configuration;
        for (int index = first+2; index < argc; index += 2) {
            const auto target = channels(ascii(argv[index]), count);
            const std::filesystem::path path(argv[index+1]);
            auto imported = import_peq_text(read(path));
            for (const auto& issue : imported.issues) {
                std::cerr << "Layer " << (index-first-2)/2+1 << ", line " << issue.line << (issue.error ? ": error: " : ": note: ") << issue.message << '\n';
            }
            if (!imported.layer) { throw std::invalid_argument("Import failed; no stack prepared"); }
            imported.layer->channels = target;
            const auto name = path.filename().u8string();
            imported.layer->name.assign(name.begin(), name.end());
            configuration.layers.push_back(std::move(*imported.layer));
        }
        ParametricEq eq;
        eq.configure(configuration, rate, count);
        if (save) { save_profile_file(argv[2], {configuration, static_cast<double>(rate), count}); }
        report(configuration, eq);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Contour EQ: " << error.what() << '\n';
        return 1;
    }
}
