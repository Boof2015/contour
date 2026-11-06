#pragma once

#include <contour/channel_gain.h>
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace contour::eq {

inline constexpr std::size_t max_layers = 32;
inline constexpr std::size_t max_filters_per_layer = 64;
inline constexpr std::size_t max_sections_per_channel = 256; // Includes layer gains.
inline constexpr double butterworth_q = 0.7071067811865475244;

enum class FilterType { peak, low_shelf, high_shelf, low_pass, high_pass, notch, all_pass, band_pass };

struct Filter {
    FilterType type = FilterType::peak;
    double frequency_hz = 1000;
    double gain_db = 0; // Used by peaks/shelves; must be zero for other types.
    double q = butterworth_q;
    bool enabled = true;
    bool operator==(const Filter&) const = default;
};

struct ChannelSelection {
    // Bit zero is interleaved channel zero. All resolves against the current
    // channel count; selected masks must be nonempty and within that layout.
    bool all = true;
    std::uint32_t mask = 0;
    static ChannelSelection selected(std::uint32_t mask) { return {false, mask}; }
    bool operator==(const ChannelSelection&) const = default;
};

struct Layer {
    std::string name;
    ChannelSelection channels;
    std::vector<Filter> filters;
    double gain_db = 0;
    bool enabled = true;
    bool operator==(const Layer&) const = default;
};

struct Configuration {
    std::vector<Layer> layers;
    bool bypass = false;
    bool operator==(const Configuration&) const = default;
};

// A frequency-grid estimate, not a transient/true-peak or clipping guarantee.
struct HeadroomEstimate {
    double peak_db;
    double peak_frequency_hz;
    double suggested_attenuation_db; // Nonnegative amount; never applied automatically.
};

struct BiquadCoefficients { double b0, b1, b2, a1, a2; };

// Shared validation for imported and manually authored settings, independent
// of the eventual device sample rate. Throws std::invalid_argument on failure.
void validate_filter(const Filter& filter);
double shelf_q_from_slope(double gain_db, double slope);
const char* filter_type_name(FilterType type) noexcept;

class ParametricEq final {
public:
    // Control thread only, while processing is stopped. Allocates, validates,
    // designs coefficients and clears history. Failure preserves the previous
    // configuration AND its history. No concurrent configuration/live updates.
    void configure(const Configuration& configuration, double sample_rate, std::size_t channels);

    // Interleaved float32, exact in-place or disjoint storage, frames * channels
    // samples available. Zero frames permits null pointers. A configured empty
    // stack is unity. No allocation, locks, I/O, or coefficient design here.
    // Silence must still be processed as zero samples to drain IIR history.
    void process(const float* input, float* output, std::size_t frames) noexcept;
    void reset() noexcept;

    std::size_t channel_count() const noexcept { return channels_.size(); }
    double sample_rate() const noexcept { return sample_rate_; }
    std::size_t section_count(std::size_t channel) const;
    // Control-thread copy for constructing a resident APO runtime bank.
    std::vector<BiquadCoefficients> coefficients(std::size_t channel) const;
    std::complex<double> response(std::size_t channel, double frequency_hz) const;
    double response_db(std::size_t channel, double frequency_hz) const;
    HeadroomEstimate estimate_headroom(std::size_t channel, std::size_t points = 8192) const;

private:
    struct Section {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1 = 0, z2 = 0;
    };
    static Section design(const Filter& filter, double sample_rate);
    std::vector<std::vector<Section>> channels_;
    std::vector<std::vector<double>> landmark_frequencies_;
    double sample_rate_ = 0;
};

} // namespace contour::eq
