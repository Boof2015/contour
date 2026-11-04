#include <contour/channel_gain.h>
#include "test_support.h"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

using contour::dsp::ChannelGain;
using test::expect_near;
using test::require;

int main() {
    test::Suite suite;
    suite.run("independent channel gains and in-place output", [] {
        ChannelGain gain;
        const std::array coefficients{0.25F, 0.75F};
        require(gain.configure(coefficients), "configure stereo");
        const std::array input{1.0F, -1.0F, 0.0F, 0.5F, -0.5F, 0.0F};
        const std::array expected{0.25F, -0.75F, 0.0F, 0.375F, -0.125F, 0.0F};
        std::array<float, 8> output{};
        output.fill(123.0F);
        gain.process(input.data(), output.data() + 1, 3);
        require(output.front() == 123.0F && output.back() == 123.0F, "buffer guards");
        for (std::size_t i = 0; i < input.size(); ++i) {
            expect_near(output[i + 1], expected[i], "independent gain");
        }
        auto inplace = input;
        gain.process(inplace.data(), inplace.data(), 3);
        require(inplace == expected, "in-place equals disjoint output");
        gain.process(nullptr, nullptr, 0);
    });
    suite.run("invalid configuration preserves previous gains", [] {
        ChannelGain gain;
        const std::array good{0.5F};
        require(gain.configure(good), "initial configure");
        require(!gain.configure({}), "reject no channels");
        std::array<float, contour::dsp::max_channels + 1> excessive{};
        require(!gain.configure(excessive), "reject excessive channels");
        for (const float bad : {-1.0F, std::numeric_limits<float>::infinity(),
                               std::numeric_limits<float>::quiet_NaN()}) {
            const std::array invalid{bad};
            require(!gain.configure(invalid), "reject invalid gain");
            float sample = 1.0F;
            gain.process(&sample, &sample, 1);
            expect_near(sample, 0.5, "previous configuration survives");
        }
    });
    suite.run("every channel stays isolated across supported layouts", [] {
        for (std::size_t channels = 1; channels <= contour::dsp::max_channels; ++channels) {
            ChannelGain gain;
            std::vector<float> coefficients(channels);
            for (std::size_t c = 0; c < channels; ++c) {
                coefficients[c] = static_cast<float>(c + 1) / 32.0F;
            }
            require(gain.configure(coefficients), "configure channel layout");
            std::vector<float> input(channels * channels, 0.0F);
            std::vector<float> output(input.size(), 99.0F);
            for (std::size_t c = 0; c < channels; ++c) {
                input[c * channels + c] = 1.0F;
            }
            gain.process(input.data(), output.data(), channels);
            for (std::size_t frame = 0; frame < channels; ++frame) {
                for (std::size_t c = 0; c < channels; ++c) {
                    expect_near(output[frame * channels + c], frame == c ? coefficients[c] : 0.0,
                         "channel impulse");
                }
            }
        }
    });
    suite.run("mute, unity, boost, and reconfiguration", [] {
        ChannelGain gain;
        const std::array coefficients{0.0F, 1.0F, 2.0F};
        require(gain.configure(coefficients), "configure three channels");
        std::array samples{0.75F, -0.5F, 0.25F};
        gain.process(samples.data(), samples.data(), 1);
        require(samples == std::array{0.0F, -0.5F, 0.5F}, "per-channel result");
        const std::array mono{0.5F};
        require(gain.configure(mono), "reconfigure mono");
        gain.process(samples.data(), samples.data(), samples.size());
        require(samples == std::array{0.0F, -0.25F, 0.25F}, "new channel count used");
    });
    return suite.finish();
}
