#include <contour/peq.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string_view>

namespace contour::eq {
namespace {
constexpr double pi = std::numbers::pi;
void check(bool condition, const char* message) {
    if (!condition) { throw std::invalid_argument(message); }
}
bool in_range(double value, double low, double high) {
    return std::isfinite(value) && value >= low && value <= high;
}
bool has_gain(FilterType type) {
    return type == FilterType::peak || type == FilterType::low_shelf || type == FilterType::high_shelf;
}
} // namespace

const char* filter_type_name(FilterType type) noexcept {
    switch (type) {
    case FilterType::peak: return "PK";
    case FilterType::low_shelf: return "LSQ";
    case FilterType::high_shelf: return "HSQ";
    case FilterType::low_pass: return "LPQ";
    case FilterType::high_pass: return "HPQ";
    case FilterType::notch: return "NO";
    case FilterType::all_pass: return "AP";
    case FilterType::band_pass: return "BP";
    default: return "Unknown";
    }
}

void validate_filter(const Filter& filter) {
    check(std::string_view(filter_type_name(filter.type)) != "Unknown", "Unknown filter type");
    check(in_range(filter.frequency_hz, 1, 192000), "Filter frequency must be 1-192000 Hz");
    check(in_range(filter.gain_db, -120, 36), "Filter gain must be -120 to +36 dB");
    check(in_range(filter.q, 0.05, 1000), "Filter Q must be 0.05-1000");
    check(has_gain(filter.type) || filter.gain_db == 0, "Gain is only defined for peak and shelf filters");
}

double shelf_q_from_slope(double gain_db, double slope) {
    check(in_range(gain_db, -120, 36) && in_range(slope, 0.01, 1), "Unsupported shelf gain or slope");
    const double a = std::pow(10.0, gain_db / 40);
    return 1 / std::sqrt((a + 1/a) * (1/slope - 1) + 2);
}

ParametricEq::Section ParametricEq::design(const Filter& f, double rate) {
    // RBJ/W3C Audio EQ Cookbook. Frequency is shelf midpoint; Q is RBJ Q.
    // Double coefficients and state protect narrow, low-frequency sections.
    const double w = 2 * pi * f.frequency_hz / rate;
    const double c = std::cos(w), alpha = std::sin(w) / (2 * f.q);
    const double a = std::pow(10.0, f.gain_db / 40);
    const double beta = 2 * std::sqrt(a) * alpha;
    Section s;
    double a0 = 1 + alpha;
    s.a1 = -2 * c;
    s.a2 = 1 - alpha;
    switch (f.type) {
    case FilterType::peak:
        s.b0 = 1 + alpha * a; s.b1 = -2 * c; s.b2 = 1 - alpha * a;
        a0 = 1 + alpha / a; s.a2 = 1 - alpha / a;
        break;
    case FilterType::low_shelf:
        s.b0 = a * ((a+1) - (a-1)*c + beta);
        s.b1 = 2*a * ((a-1) - (a+1)*c);
        s.b2 = a * ((a+1) - (a-1)*c - beta);
        a0 = (a+1) + (a-1)*c + beta;
        s.a1 = -2 * ((a-1) + (a+1)*c);
        s.a2 = (a+1) + (a-1)*c - beta;
        break;
    case FilterType::high_shelf:
        s.b0 = a * ((a+1) + (a-1)*c + beta);
        s.b1 = -2*a * ((a-1) + (a+1)*c);
        s.b2 = a * ((a+1) + (a-1)*c - beta);
        a0 = (a+1) - (a-1)*c + beta;
        s.a1 = 2 * ((a-1) - (a+1)*c);
        s.a2 = (a+1) - (a-1)*c - beta;
        break;
    case FilterType::low_pass:
        s.b0 = (1-c)/2; s.b1 = 1-c; s.b2 = s.b0;
        break;
    case FilterType::high_pass:
        s.b0 = (1+c)/2; s.b1 = -(1+c); s.b2 = s.b0;
        break;
    case FilterType::notch:
        s.b0 = 1; s.b1 = -2*c; s.b2 = 1;
        break;
    case FilterType::all_pass:
        s.b0 = 1-alpha; s.b1 = -2*c; s.b2 = 1+alpha;
        break;
    case FilterType::band_pass:
        s.b0 = alpha; s.b1 = 0; s.b2 = -alpha;
        break;
    default: throw std::invalid_argument("Unknown filter type");
    }
    s.b0 /= a0; s.b1 /= a0; s.b2 /= a0; s.a1 /= a0; s.a2 /= a0;
    check(std::isfinite(s.b0) && std::isfinite(s.b1) && std::isfinite(s.b2) &&
          std::isfinite(s.a1) && std::isfinite(s.a2), "Non-finite filter coefficients");
    // Strict second-order Jury stability conditions for 1 + a1*z^-1 + a2*z^-2.
    check(std::abs(s.a2) < 1 && 1+s.a1+s.a2 > 0 && 1-s.a1+s.a2 > 0,
          "Filter is not numerically stable at this sample rate");
    if (has_gain(f.type) && f.gain_db != 0) {
        // Very deep, extremely narrow cuts near DC can lose their intended
        // depth through coefficient cancellation even with stable poles.
        const auto z = std::polar(1.0, -w);
        const double actual = 20*std::log10(std::abs((s.b0+s.b1*z+s.b2*z*z)/(1.0+s.a1*z+s.a2*z*z)));
        const double intended = f.type == FilterType::peak ? f.gain_db : f.gain_db/2;
        check(std::isfinite(actual) && std::abs(actual-intended) <= 0.01,
              "Filter cannot meet 0.01 dB center accuracy at this rate; reduce extreme gain/Q or move frequency away from DC/Nyquist");
    }
    return s;
}

void ParametricEq::configure(const Configuration& config, double rate, std::size_t count) {
    check(in_range(rate, 8000, 384000), "Sample rate must be 8000-384000 Hz");
    check(count >= 1 && count <= dsp::max_channels, "Channel count must be 1-32");
    check(config.layers.size() <= max_layers, "Too many EQ layers (maximum 32)");
    std::vector<std::vector<Section>> next(count);
    std::vector<std::vector<double>> landmarks(count);
    const auto allowed = count == 32 ? UINT32_MAX : (std::uint32_t{1} << count) - 1;
    for (std::size_t index = 0; index < config.layers.size(); ++index) {
        const auto& layer = config.layers[index];
        try {
            check(layer.channels.all ? layer.channels.mask == 0 :
                layer.channels.mask != 0 && (layer.channels.mask & ~allowed) == 0,
                "Select All or a nonempty subset of channels in this layout");
            check(in_range(layer.gain_db, -120, 36), "Layer gain must be -120 to +36 dB");
            check(layer.filters.size() <= max_filters_per_layer, "Too many filters in layer (maximum 64)");
            std::vector<Section> sections;
            std::vector<double> frequencies;
            if (layer.gain_db != 0) {
                Section gain;
                gain.b0 = std::pow(10.0, layer.gain_db / 20);
                sections.push_back(gain);
            }
            for (std::size_t filter_index = 0; filter_index < layer.filters.size(); ++filter_index) {
                const auto& f = layer.filters[filter_index];
                try {
                    validate_filter(f);
                    check(f.frequency_hz < rate / 2, "Filter frequency must be below Nyquist for this sample rate");
                    const auto section = design(f, rate);
                    if (f.enabled && !(has_gain(f.type) && f.gain_db == 0)) {
                        sections.push_back(section);
                        frequencies.push_back(f.frequency_hz);
                    }
                } catch (const std::invalid_argument& error) {
                    throw std::invalid_argument("Filter " + std::to_string(filter_index + 1) + ": " + error.what());
                }
            }
            if (!layer.enabled || config.bypass) { continue; }
            for (std::size_t channel = 0; channel < count; ++channel) {
                if (layer.channels.all || (layer.channels.mask & (std::uint32_t{1} << channel))) {
                    check(next[channel].size() + sections.size() <= max_sections_per_channel,
                          "Too many active sections on a channel (maximum 256 including gains)");
                    next[channel].insert(next[channel].end(), sections.begin(), sections.end());
                    landmarks[channel].insert(landmarks[channel].end(), frequencies.begin(), frequencies.end());
                }
            }
        } catch (const std::invalid_argument& error) {
            throw std::invalid_argument("Layer " + std::to_string(index + 1) + ": " + error.what());
        }
    }
    channels_.swap(next);
    landmark_frequencies_.swap(landmarks);
    sample_rate_ = rate;
}

#if defined(_MSC_VER)
#pragma code_seg(push, "RT_CODE")
#endif
void ParametricEq::process(const float* input, float* output, std::size_t frames) noexcept {
    if (frames == 0) { return; }
    const auto count = channels_.size();
    for (std::size_t channel = 0; channel < count; ++channel) {
        auto& sections = channels_[channel];
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto position = frame * count + channel;
            double value = std::isfinite(input[position]) ? input[position] : 0;
            for (auto& s : sections) {
                const double y = s.b0 * value + s.z1;
                s.z1 = s.b1 * value - s.a1 * y + s.z2;
                s.z2 = s.b2 * value - s.a2 * y;
                value = y;
            }
            if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) {
                // An invalid/extreme sample must not poison an IIR indefinitely.
                for (auto& s : sections) { s.z1 = s.z2 = 0; }
                value = 0;
            }
            output[position] = static_cast<float>(value);
        }
        for (auto& s : sections) {
            if (std::abs(s.z1) < 1e-30) { s.z1 = 0; }
            if (std::abs(s.z2) < 1e-30) { s.z2 = 0; }
        }
    }
}

void ParametricEq::reset() noexcept {
    for (auto& channel : channels_) { for (auto& s : channel) { s.z1 = s.z2 = 0; } }
}
#if defined(_MSC_VER)
#pragma code_seg(pop)
#endif

std::size_t ParametricEq::section_count(std::size_t channel) const { return channels_.at(channel).size(); }

std::vector<BiquadCoefficients> ParametricEq::coefficients(std::size_t channel) const {
    std::vector<BiquadCoefficients> result;
    for (const auto& s : channels_.at(channel)) { result.push_back({s.b0,s.b1,s.b2,s.a1,s.a2}); }
    return result;
}

std::complex<double> ParametricEq::response(std::size_t channel, double frequency) const {
    const auto& sections = channels_.at(channel);
    check(in_range(frequency, 0, sample_rate_ / 2), "Response frequency must be between DC and Nyquist");
    const auto z = std::polar(1.0, -2*pi*frequency/sample_rate_);
    std::complex<double> result = 1;
    for (const auto& s : sections) { result *= (s.b0 + s.b1*z + s.b2*z*z) / (1.0 + s.a1*z + s.a2*z*z); }
    return result;
}

double ParametricEq::response_db(std::size_t channel, double frequency) const {
    // Sum logarithms so a long cascade does not under/overflow the product.
    const auto& sections = channels_.at(channel);
    check(in_range(frequency, 0, sample_rate_ / 2), "Response frequency must be between DC and Nyquist");
    const auto z = std::polar(1.0, -2*pi*frequency/sample_rate_);
    double result = 0;
    for (const auto& s : sections) {
        const auto h = (s.b0 + s.b1*z + s.b2*z*z) / (1.0 + s.a1*z + s.a2*z*z);
        result += 20 * std::log10(std::max(std::abs(h), 1e-15));
    }
    return result;
}

HeadroomEstimate ParametricEq::estimate_headroom(std::size_t channel, std::size_t points) const {
    check(points >= 16 && points <= 65536, "Headroom grid must have 16-65536 points");
    HeadroomEstimate result{response_db(channel, 0), 0, 0};
    const auto consider = [&](double frequency) {
        const double db = response_db(channel, frequency);
        if (db > result.peak_db) { result.peak_db = db; result.peak_frequency_hz = frequency; }
    };
    const double nyquist = sample_rate_/2;
    for (std::size_t i = 0; i < points; ++i) {
        consider(std::min(nyquist, std::exp(std::log(nyquist) * static_cast<double>(i) / static_cast<double>(points-1))));
    }
    consider(nyquist);
    for (const double frequency : landmark_frequencies_.at(channel)) { consider(frequency); }
    result.suggested_attenuation_db = std::max(0.0, result.peak_db);
    return result;
}

} // namespace contour::eq
