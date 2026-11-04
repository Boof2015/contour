#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace contour::dsp {

// Fixed storage keeps the processing path independent of an allocator.
inline constexpr std::size_t max_channels = 32;

class ChannelGain final {
public:
    // Configuration is non-real-time and must not race with process(). An
    // invalid configuration leaves the last valid configuration intact.
    [[nodiscard]] bool configure(std::span<const float> linear_gains) noexcept;

    // Interleaved float32; buffers contain at least frames * channels samples.
    // Exact in-place or disjoint buffers only. Configure before processing.
    // No allocation, synchronization, I/O, or coefficient calculation here.
    void process(const float* input, float* output, std::size_t frames) const noexcept;

private:
    std::array<float, max_channels> gains_{};
    std::size_t channels_ = 0;
};

} // namespace contour::dsp
