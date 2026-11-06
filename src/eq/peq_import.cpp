#include <contour/peq_import.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <regex>
#include <set>
#include <stdexcept>

namespace contour::eq {
namespace {
std::string trim(std::string_view text) {
    const auto begin = text.find_first_not_of(" \t\r");
    if (begin == text.npos) { return {}; }
    return std::string(text.substr(begin, text.find_last_not_of(" \t\r") - begin + 1));
}
std::string upper(std::string text) {
    for (auto& c : text) { if (c >= 'a' && c <= 'z') { c = static_cast<char>(c - 'a' + 'A'); } }
    return text;
}
double number(std::string_view text) {
    if (text.starts_with('+')) { text.remove_prefix(1); }
    double value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || !std::isfinite(value)) {
        throw std::invalid_argument("Invalid finite number");
    }
    return value;
}
} // namespace

ImportResult import_peq_text(std::string_view text) {
    ImportResult result;
    if (text.size() > 1024 * 1024) { return {{}, {{0, true, "PEQ text exceeds 1 MiB"}}}; }
    if (text.starts_with("\xEF\xBB\xBF")) { text.remove_prefix(3); }
    if (text.starts_with("\xFF\xFE") || text.starts_with("\xFE\xFF") || text.find('\0') != text.npos) {
        return {{}, {{0, true, "Use UTF-8/plain text; UTF-16 and binary files are not supported"}}};
    }
    // Anchored grammar deliberately rejects ignored tails, nonfinite numbers,
    // ambiguous shelves, bandwidth conventions and unsupported commands.
    const std::string n = R"(([+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?))";
    const std::regex filter_line(R"(^Filter(?:\s+([0-9]+))?\s*:\s*(ON|OFF)\s+(.+)$)", std::regex::icase);
    const std::regex parameters("^(PK|PEQ|LSC|HSC|LSQ|HSQ|LS|HS|LPQ|HPQ|LP|HP|NO|AP|BP)\\s+Fc\\s+" + n +
        "\\s*Hz(?:\\s+Gain\\s+" + n + "\\s*dB)?(?:\\s+Q\\s+" + n + ")?\\s*$", std::regex::icase);
    const std::regex preamp("^Preamp\\s*:\\s*" + n + "\\s*dB$", std::regex::icase);
    Layer layer;
    bool header = false, seen_filter = false, seen_preamp = false;
    std::set<unsigned> ids;
    std::size_t line_number = 0, filter_rows = 0;
    while (!text.empty()) {
        ++line_number;
        const auto end = text.find('\n');
        auto line = trim(text.substr(0, end));
        text.remove_prefix(end == text.npos ? text.size() : end + 1);
        if (line.size() > 4096) { result.issues.push_back({line_number, true, "Line exceeds 4096 bytes"}); continue; }
        line = trim(std::string_view(line).substr(0, line.find('#')));
        if (line.empty()) { continue; }
        const auto caps = upper(line);
        std::smatch match;
        try {
            if (caps == "FILTER SETTINGS FILE" && !seen_filter) { header = true; continue; }
            if (caps.starts_with("EQUALISER:") || caps.starts_with("EQUALIZER:")) {
                const auto value = trim(std::string_view(caps).substr(caps.find(':') + 1));
                if (value != "GENERIC" && value != "EQUALIZER APO") {
                    throw std::invalid_argument("Unsupported equaliser convention; export Generic or Equalizer APO PEQ text");
                }
                continue;
            }
            if (std::regex_match(line, match, preamp)) {
                if (seen_preamp || seen_filter) { throw std::invalid_argument("Use one Preamp line before the filters"); }
                layer.gain_db = number(match[1].str());
                if (layer.gain_db < -120 || layer.gain_db > 36) { throw std::invalid_argument("Preamp must be -120 to +36 dB"); }
                seen_preamp = true;
                continue;
            }
            if (std::regex_match(line, match, filter_line)) {
                seen_filter = true;
                if (++filter_rows > max_filters_per_layer) { throw std::invalid_argument("Too many filter rows (maximum 64)"); }
                const auto body = match[3].str();
                // REW Generic exports can append unused slots numbered 1 and 2
                // again. They describe no filter and do not reserve a number.
                if (upper(body) == "NONE") { continue; }
                if (match[1].matched) {
                    const double id = number(match[1].str());
                    if (id < 1 || id > 1000000 || !ids.insert(static_cast<unsigned>(id)).second) {
                        throw std::invalid_argument("Filter numbers must be positive and unique");
                    }
                }
                const bool enabled = upper(match[2].str()) == "ON";
                std::smatch fields;
                if (!std::regex_match(body, fields, parameters)) {
                    throw std::invalid_argument("Unsupported or malformed filter; expected type, Fc/Hz, Gain/dB if applicable, and Q. Corner/slope shelves and BW are not supported");
                }
                const auto type = upper(fields[1].str());
                Filter filter;
                filter.enabled = enabled;
                filter.frequency_hz = number(fields[2].str());
                const bool gain_present = fields[3].matched, q_present = fields[4].matched;
                if (type == "PK" || type == "PEQ") { filter.type = FilterType::peak; }
                else if (type == "LS" || type == "LSC" || type == "LSQ") { filter.type = FilterType::low_shelf; }
                else if (type == "HS" || type == "HSC" || type == "HSQ") { filter.type = FilterType::high_shelf; }
                else if (type == "LP" || type == "LPQ") { filter.type = FilterType::low_pass; }
                else if (type == "HP" || type == "HPQ") { filter.type = FilterType::high_pass; }
                else if (type == "NO") { filter.type = FilterType::notch; }
                else if (type == "AP") { filter.type = FilterType::all_pass; }
                else { filter.type = FilterType::band_pass; }
                const bool shelf = filter.type == FilterType::low_shelf || filter.type == FilterType::high_shelf;
                const bool uses_gain = shelf || filter.type == FilterType::peak;
                if (gain_present != uses_gain) { throw std::invalid_argument("Gain must appear exactly for peak/shelf filters"); }
                if (gain_present) { filter.gain_db = number(fields[3].str()); }
                if (q_present) { filter.q = number(fields[4].str()); }
                else if (type == "LS" || type == "HS" || type == "LSC" || type == "HSC") {
                    filter.q = shelf_q_from_slope(filter.gain_db, 0.9);
                } else if (type != "LP" && type != "HP") {
                    throw std::invalid_argument("This filter requires an explicit Q value");
                }
                validate_filter(filter);
                layer.filters.push_back(filter);
                continue;
            }
            if (header && !seen_filter && (caps.starts_with("ROOM EQ ") || caps.starts_with("DATED:") ||
                caps.starts_with("NOTES:") || caps.starts_with("TARGET LEVEL:"))) { continue; }
            // REW includes unlabelled measurement/target descriptions before the
            // first filter. Retain a visible warning when accepting those lines.
            if (header && !seen_filter && line.find(':') == line.npos && !caps.starts_with("FILTER") &&
                !caps.starts_with("PREAMP")) {
                result.issues.push_back({line_number, false, "Ignored REW description: " + line});
                continue;
            }
            throw std::invalid_argument("Unrecognised line or unsupported command; routing and script commands are not PEQ filters");
        } catch (const std::invalid_argument& error) {
            result.issues.push_back({line_number, true, error.what()});
        }
    }
    if (!seen_filter && !seen_preamp) { result.issues.push_back({0, true, "No filter or preamp settings found"}); }
    if (std::none_of(result.issues.begin(), result.issues.end(), [](const auto& issue) { return issue.error; })) {
        result.layer = std::move(layer);
    }
    return result;
}

} // namespace contour::eq
