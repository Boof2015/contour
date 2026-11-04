#include <contour/channel_gain.h>

#include <cmath>

namespace contour::dsp {

bool ChannelGain::configure(std::span<const float> linear_gains) noexcept {
    if (linear_gains.empty() || linear_gains.size() > max_channels) {
        return false;
    }
    for (const float gain : linear_gains) {
        if (!std::isfinite(gain) || gain < 0.0F) {
            return false;
        }
    }
    channels_ = linear_gains.size();
    for (std::size_t channel = 0; channel < channels_; ++channel) {
        gains_[channel] = linear_gains[channel];
    }
    return true;
}

#if defined(_MSC_VER)
#pragma code_seg(push, "RT_CODE")
#endif
void ChannelGain::process(const float* input, float* output, std::size_t frames) const noexcept {
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t channel = 0; channel < channels_; ++channel) {
            const auto sample = frame * channels_ + channel;
            output[sample] = input[sample] * gains_[channel];
        }
    }
}
#if defined(_MSC_VER)
#pragma code_seg(pop)
#endif

} // namespace contour::dsp
