#pragma once
#include <contour/peq.h>
#include <atomic>

namespace contour::eq {

// Each bank is one AERT allocation containing its header, coefficients and
// state. All compilation and reclamation happen on the control thread.
struct ResidentBank {
    static ResidentBank* prepare(const Configuration&, double rate, std::size_t channels, std::uint64_t revision);
    static void release(ResidentBank*) noexcept;
    float sample(std::size_t channel, float input) noexcept;
    void reset() noexcept;
    std::uint64_t revision = 0;
    std::uint32_t channels = 0;
    std::uint32_t offsets[dsp::max_channels+1]{};
private:
    struct Section { BiquadCoefficients c; double z1 = 0, z2 = 0; };
    Section* sections() noexcept { return reinterpret_cast<Section*>(this+1); }
};

class SmoothEq final {
public:
    SmoothEq() = default;
    ~SmoothEq() { clear(); }
    SmoothEq(const SmoothEq&) = delete;
    SmoothEq& operator=(const SmoothEq&) = delete;
    // Processing stopped and publisher stopped for initialize/clear.
    void initialize(ResidentBank* initial, std::uint32_t fade_frames) noexcept;
    void clear() noexcept;
    // One control-thread publisher. Takes ownership, coalesces queued updates.
    bool publish(ResidentBank* bank) noexcept;
    void reclaim() noexcept;
    // One audio-thread consumer. Null input is logical zero, never stale data.
    // Returns true if any emitted sample is nonzero. No allocation or freeing.
    bool process(const float* input, float* output, std::size_t frames) noexcept;
    // Serialized with the audio callback, as required by the APO Reset contract.
    void reset() noexcept;
    std::uint64_t applied_revision() const noexcept { return applied_.load(std::memory_order_acquire); }
private:
    ResidentBank* current_ = nullptr;
    ResidentBank* incoming_ = nullptr;
    std::atomic<ResidentBank*> pending_{nullptr};
    std::atomic<ResidentBank*> retired_{nullptr};
    std::atomic<std::uint64_t> applied_{0};
    std::uint32_t fade_frames_ = 2, progress_ = 0;
    std::uint32_t channels_ = 0;
};
static_assert(std::atomic<ResidentBank*>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
} // namespace contour::eq
