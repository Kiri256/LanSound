#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

namespace lanaudio {
struct Stereo { float left = 0, right = 0; };

// Exactly one producer and one consumer. Only the consumer advances read_.
// A full queue rejects new data; it never overwrites samples being read.
template<size_t Capacity> class FrameRing {
    static_assert((Capacity & (Capacity - 1)) == 0);
public:
    bool write(const Stereo* input, size_t count) {
        const auto w = write_.load(std::memory_order_relaxed);
        const auto r = read_.load(std::memory_order_acquire);
        if (count > Capacity || w - r + count > Capacity) return false;
        if (!count) return true;
        const size_t offset = w & (Capacity - 1);
        const size_t first = std::min(count, Capacity - offset);
        std::memcpy(data_.data() + offset, input, first * sizeof(Stereo));
        if (first < count) std::memcpy(data_.data(), input + first, (count - first) * sizeof(Stereo));
        write_.store(w + count, std::memory_order_release);
        return true;
    }
    uint64_t end() const { return write_.load(std::memory_order_acquire); }
    uint64_t begin() const { return read_.load(std::memory_order_acquire); }
    Stereo at(uint64_t index) const { return data_[index & (Capacity - 1)]; }
    // Consumer may load a contiguous group without crossing the ring boundary.
    const Stereo* contiguous(uint64_t index, size_t count) const {
        const size_t offset = index & (Capacity - 1);
        return count <= Capacity - offset ? data_.data() + offset : nullptr;
    }
    void release(uint64_t index) { read_.store(index, std::memory_order_release); }
private:
    std::array<Stereo, Capacity> data_{};
    alignas(64) std::atomic<uint64_t> write_{0};
    alignas(64) std::atomic<uint64_t> read_{0};
};

// Precomputed 96-tap Blackman-windowed sinc, interpolated between 256 phases.
// Constructed outside the callback. Cutoff covers source/output rate conversion.
class SincResampler {
public:
    static constexpr int Half = 48, Taps = Half * 2, Phases = 256;
    explicit SincResampler(double sourcePerOutput) {
        const double cutoff = 0.94 / std::max(1.0, sourcePerOutput * 1.002);
        constexpr double pi = 3.14159265358979323846;
        for (int p = 0; p <= Phases; ++p) {
            double sum = 0;
            for (int t = 0; t < Taps; ++t) {
                const double x = t - (Half - 1) - double(p) / Phases;
                const double window = std::abs(x) >= Half ? 0 :
                    0.42 + 0.5 * std::cos(pi * x / Half) + 0.08 * std::cos(2 * pi * x / Half);
                const double sinc = std::abs(x) < 1e-10 ? cutoff : std::sin(pi * cutoff * x) / (pi * x);
                coefficients_[p][t] = float(sinc * window);
                sum += coefficients_[p][t];
            }
            for (auto& c : coefficients_[p]) c = float(c / sum);
        }
    }
    template<size_t N> Stereo sample(const FrameRing<N>& ring, uint64_t center, double fraction) const {
        const double phase = fraction * Phases;
        const int p = std::min(int(phase), Phases - 1);
        const float blend = float(phase - p);
        Stereo out;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        float32x4_t leftAcc = vdupq_n_f32(0.0f);
        float32x4_t rightAcc = vdupq_n_f32(0.0f);
        static_assert(sizeof(Stereo) == 2 * sizeof(float));
        for (int t = 0; t < Taps; t += 4) {
            const auto index = center - (Half - 1) + t;
            auto* samples = ring.contiguous(index, 4);
            std::array<Stereo, 4> wrapped;
            if (!samples) {
                for (int lane = 0; lane < 4; ++lane) wrapped[lane] = ring.at(index + lane);
                samples = wrapped.data();
            }
            const auto stereo = vld2q_f32(reinterpret_cast<const float*>(samples));
            const auto a = vld1q_f32(coefficients_[p].data() + t);
            const auto b = vld1q_f32(coefficients_[p + 1].data() + t);
            const auto c = vmlaq_n_f32(a, vsubq_f32(b, a), blend);
            leftAcc = vmlaq_f32(leftAcc, stereo.val[0], c);
            rightAcc = vmlaq_f32(rightAcc, stereo.val[1], c);
        }
        float32x2_t leftPair = vadd_f32(vget_low_f32(leftAcc), vget_high_f32(leftAcc));
        float32x2_t rightPair = vadd_f32(vget_low_f32(rightAcc), vget_high_f32(rightAcc));
        out.left = vget_lane_f32(vpadd_f32(leftPair, leftPair), 0);
        out.right = vget_lane_f32(vpadd_f32(rightPair, rightPair), 0);
#else
        for (int t = 0; t < Taps; ++t) {
            const auto s = ring.at(center - (Half - 1) + t);
            const float c = coefficients_[p][t] + blend * (coefficients_[p + 1][t] - coefficients_[p][t]);
            out.left += s.left * c;
            out.right += s.right * c;
        }
#endif
        return out;
    }
private:
    std::array<std::array<float, Taps>, Phases + 1> coefficients_{};
};

// Single-threaded packet reorder window; payload delivery waits for a missing
// packet until its deadline. Sequence arithmetic also handles uint32 wrap.
template<class Packet, size_t Capacity = 128> class ReorderBuffer {
public:
    enum class Insert { Accepted, Late, Duplicate, TooFar };
    Insert insert(uint32_t sequence, const Packet& packet) {
        if (!initialized_) { expected_ = sequence; initialized_ = true; }
        const int32_t distance = int32_t(sequence - expected_);
        if (distance < 0) return Insert::Late;
        if (uint32_t(distance) >= Capacity) return Insert::TooFar;
        auto& slot = slots_[sequence % Capacity];
        if (slot.present) return Insert::Duplicate;
        slot.packet = packet; slot.present = true; ++pending_;
        return Insert::Accepted;
    }
    bool pop(Packet& packet) {
        if (!initialized_) return false;
        auto& slot = slots_[expected_ % Capacity];
        if (!slot.present) return false;
        packet = slot.packet; slot.present = false; ++expected_; --pending_;
        return true;
    }
    bool hasFuture() const { return pending_ != 0; }
    void skip() { ++expected_; }
    void reset() { for (auto& s : slots_) s.present = false; initialized_ = false; pending_ = 0; }
private:
    struct Slot { Packet packet{}; bool present = false; };
    std::array<Slot, Capacity> slots_{};
    bool initialized_ = false;
    size_t pending_ = 0;
    uint32_t expected_ = 0;
};
}
