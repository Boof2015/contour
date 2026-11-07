#include <contour/peq_export.h>

#include <array>
#include <charconv>
#include <cmath>
#include <stdexcept>

namespace contour::eq {
namespace {
std::string number(double value) {
    std::array<char,64> buffer{};
    const auto result=std::to_chars(buffer.data(),buffer.data()+buffer.size(),value);
    if (result.ec!=std::errc{}) { throw std::invalid_argument("Cannot export filter value"); }
    return {buffer.data(),result.ptr};
}
}
std::string export_peq_text(const Layer& layer) {
    if (!std::isfinite(layer.gain_db) || layer.gain_db < -120 || layer.gain_db > 36 ||
        layer.filters.size()>max_filters_per_layer) { throw std::invalid_argument("Invalid layer for filter export"); }
    // Documented center-frequency shelves with explicit Q. LSQ/HSQ are
    // accepted by our importer but are not Equalizer APO's output syntax.
    constexpr std::array types{"PK","LSC","HSC","LPQ","HPQ","NO","AP","BP"};
    std::string result="# Contour filter export\r\n"
        "# Channel assignments and the layer on/off switch are not included.\r\n"
        "Preamp: "+number(layer.gain_db)+" dB\r\n";
    std::size_t index=0;
    for (const auto& filter:layer.filters) {
        validate_filter(filter);
        const auto type=static_cast<std::size_t>(filter.type);
        if (type>=types.size()) { throw std::invalid_argument("Unknown filter type"); }
        result+="Filter "+std::to_string(++index)+": "+(filter.enabled?"ON ":"OFF ")+types[type]+
            " Fc "+number(filter.frequency_hz)+" Hz";
        if (filter.type==FilterType::peak || filter.type==FilterType::low_shelf || filter.type==FilterType::high_shelf) {
            result+=" Gain "+number(filter.gain_db)+" dB";
        }
        result+=" Q "+number(filter.q)+"\r\n";
    }
    return result;
}
} // namespace contour::eq
