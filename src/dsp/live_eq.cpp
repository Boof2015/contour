#include <contour/live_eq.h>
#include <windows.h>
#include <baseaudioprocessingobject.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <new>

namespace contour::eq {
ResidentBank* ResidentBank::prepare(const Configuration& config, double rate, std::size_t count, std::uint64_t revision) {
    ParametricEq compiled;
    compiled.configure(config, rate, count);
    std::size_t total = 0;
    for (std::size_t c = 0; c < count; ++c) { total += compiled.section_count(c); }
    void* allocation = nullptr;
    if (FAILED(AERT_Allocate(sizeof(ResidentBank)+total*sizeof(Section), &allocation))) { throw std::bad_alloc(); }
    auto* bank = new (allocation) ResidentBank{};
    bank->revision = revision;
    bank->channels = static_cast<std::uint32_t>(count);
    try {
        std::uint32_t index = 0;
        for (std::size_t c = 0; c < count; ++c) {
            bank->offsets[c] = index;
            for (const auto& coefficients : compiled.coefficients(c)) {
                new (bank->sections()+index++) Section{coefficients};
            }
        }
        bank->offsets[count] = index;
    } catch (...) { release(bank); throw; }
    return bank;
}
void ResidentBank::release(ResidentBank* bank) noexcept { if (bank) { AERT_Free(bank); } }

void SmoothEq::initialize(ResidentBank* initial, std::uint32_t frames) noexcept {
    clear(); current_ = initial; fade_frames_ = std::max(2u, frames); channels_ = initial->channels;
    applied_.store(initial->revision, std::memory_order_release);
}
void SmoothEq::clear() noexcept {
    ResidentBank::release(current_); current_ = nullptr;
    ResidentBank::release(incoming_); incoming_ = nullptr;
    ResidentBank::release(pending_.exchange(nullptr));
    reclaim(); progress_ = 0; channels_ = 0;
    applied_.store(0, std::memory_order_release);
}
bool SmoothEq::publish(ResidentBank* bank) noexcept {
    if (!bank || bank->channels != channels_) { ResidentBank::release(bank); return false; }
    ResidentBank::release(pending_.exchange(bank, std::memory_order_acq_rel));
    reclaim();
    return true;
}
void SmoothEq::reclaim() noexcept { ResidentBank::release(retired_.exchange(nullptr, std::memory_order_acq_rel)); }

#pragma AVRT_CODE_BEGIN
float ResidentBank::sample(std::size_t channel, float input) noexcept {
    double value = std::isfinite(input) ? input : 0;
    auto* data = sections();
    for (auto i = offsets[channel]; i < offsets[channel+1]; ++i) {
        auto& s = data[i];
        const double y = s.c.b0*value+s.z1;
        s.z1 = s.c.b1*value-s.c.a1*y+s.z2;
        s.z2 = s.c.b2*value-s.c.a2*y;
        if (std::abs(s.z1) < 1e-30) { s.z1 = 0; }
        if (std::abs(s.z2) < 1e-30) { s.z2 = 0; }
        value = y;
    }
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) {
        for (auto i = offsets[channel]; i < offsets[channel+1]; ++i) { data[i].z1 = data[i].z2 = 0; }
        value = 0;
    }
    return static_cast<float>(value);
}
void ResidentBank::reset() noexcept {
    for (std::uint32_t i = 0; i < offsets[channels]; ++i) { sections()[i].z1 = sections()[i].z2 = 0; }
}
void SmoothEq::reset() noexcept {
    if (current_) { current_->reset(); }
    if (incoming_) { incoming_->reset(); }
}
bool SmoothEq::process(const float* input, float* output, std::size_t frames) noexcept {
    if (!current_ || frames == 0) { return false; }
    if (!incoming_ && retired_.load(std::memory_order_acquire) == nullptr) {
        incoming_ = pending_.exchange(nullptr, std::memory_order_acq_rel);
        progress_ = 0;
    }
    bool audible = false;
    const auto count = current_->channels;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const double weight = static_cast<double>(progress_)/static_cast<double>(fade_frames_-1);
        for (std::size_t channel = 0; channel < count; ++channel) {
            const auto index = frame*count+channel;
            const float source = input ? input[index] : 0;
            float value = current_->sample(channel, source);
            if (incoming_) {
                const float next = incoming_->sample(channel, source);
                // Convex interpolation avoids an equal-power gain bump for
                // correlated versions of the same signal.
                value = static_cast<float>((1-weight)*value + weight*next);
            }
            output[index] = value;
            audible = audible || value != 0;
        }
        if (incoming_ && ++progress_ == fade_frames_) {
            auto* previous = current_;
            current_ = incoming_; incoming_ = nullptr;
            applied_.store(current_->revision, std::memory_order_release);
            retired_.store(previous, std::memory_order_release);
        }
    }
    return audible;
}
#pragma AVRT_CODE_END
} // namespace contour::eq
