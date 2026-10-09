#pragma once
#include "audio_core.h"
#include "audio_metrics.h"
#include "audio_protocol.h"

namespace lanaudio {
// Receiver owns write(); the output callback alone owns render() and cursors.
// Construction happens before playback starts; render() allocates and locks nothing.
class PlaybackBuffer {
public:
    static constexpr int TargetMs = 100, MaxBufferedMs = 150;
    PlaybackBuffer(int sourceRate, int outputRate, Metrics& metrics)
        : sourceRate_(sourceRate), outputRate_(outputRate), metrics_(metrics),
          target_(sourceRate * TargetMs / 1000), ratio_(double(sourceRate) / outputRate), filter_(ratio_) {}
    void write(const Stereo* samples, size_t count) {
        if (!ring_.write(samples, count)) {
            metrics_.queueDrops.fetch_add(1, std::memory_order_relaxed);
            recordDrop(count);
        }
    }
    void silence() {
        constexpr std::array<Stereo, PacketFrames> zeros{};
        write(zeros.data(), zeros.size());
    }
    void reset() { resetBoundary_.store(ring_.end(), std::memory_order_release); }
    int targetMs() const { return TargetMs; }
    int correctionPpm() const { return correctionPpm_.load(std::memory_order_relaxed); }
    uint64_t bufferedMs() const { return buffered_.load(std::memory_order_relaxed) * 1000 / sourceRate_; }
    void render(float* output, int frames) {
        const auto boundary = resetBoundary_.load(std::memory_order_acquire);
        const auto end = ring_.end();
        constexpr uint64_t history = SincResampler::Half - 1;
        const uint64_t lookahead = SincResampler::Half + 1 + uint64_t(std::ceil(ratio_ * 1.002));
        if (boundary != handledBoundary_) {
            const auto next = std::max(boundary, ring_.begin());
            if (next > ring_.begin()) recordDrop(next - ring_.begin());
            ring_.release(next); cursor_ = std::max(cursor_, next); started_ = false; phase_ = 0;
            filteredDepth_ = 0; correction_ = 0; fade_ = 0;
            handledBoundary_ = boundary;
        }
        if (!started_ && end - ring_.begin() >= target_ + history + lookahead) {
            const auto next = end - target_;
            if (everStarted_ && next > cursor_) recordDrop(next - cursor_);
            seek(next); started_ = true; everStarted_ = true;
        }
        if (started_ && end - cursor_ > uint64_t(sourceRate_) * MaxBufferedMs / 1000) {
            const auto next = end - target_;
            recordDrop(next - cursor_);
            metrics_.activeSkips.fetch_add(1, std::memory_order_relaxed);
            seek(next);
        }
        if (started_) {
            const double alpha = 1 - std::exp(-double(frames) / outputRate_);
            filteredDepth_ += alpha * (double(end - cursor_) - filteredDepth_);
            const double error = (filteredDepth_ - target_) / sourceRate_;
            const double desired = std::abs(error) <= 0.001 ? 0 : std::clamp(error * 0.05, -0.002, 0.002);
            correction_ += alpha * (desired - correction_);
            if (desired == 0 && std::abs(correction_) < 1e-8) correction_ = 0;
        }
        const bool direct = sourceRate_ == outputRate_ && correction_ == 0 && phase_ == 0;
        correctionPpm_.store(int(std::lround(correction_ * 1000000)), std::memory_order_relaxed);
        uint64_t missing = 0;
        auto availableEnd = end;
        for (int i = 0; i < frames; ++i) {
            // Published samples stay readable until this consumer releases them.
            // Refresh only near exhaustion so arrivals during the callback can play.
            if (started_ && availableEnd - cursor_ < lookahead) availableEnd = ring_.end();
            if (!started_ || availableEnd - cursor_ < lookahead) {
                started_ = false;
                if (everStarted_) ++missing;
                last_.left *= 0.96f; last_.right *= 0.96f;
            } else {
                const auto sample = direct ? ring_.at(cursor_) : filter_.sample(ring_, cursor_, phase_);
                if (fade_ < 128) {
                    const float gain = float(++fade_) / 128;
                    last_.left += (sample.left - last_.left) * gain;
                    last_.right += (sample.right - last_.right) * gain;
                } else {
                    last_ = sample;
                }
                if (direct) {
                    ++cursor_;
                } else {
                    phase_ += ratio_ * (1 + correction_);
                    const auto advance = uint64_t(phase_);
                    cursor_ += advance; phase_ -= double(advance);
                }
            }
            output[i * 2] = last_.left; output[i * 2 + 1] = last_.right;
        }
        if (cursor_ >= history && cursor_ - history >= ring_.begin()) ring_.release(cursor_ - history);
        if (missing) {
            metrics_.missingNs.fetch_add(missing * 1000000000 / outputRate_, std::memory_order_relaxed);
            if (!wasUnderrun_) metrics_.underruns.fetch_add(1, std::memory_order_relaxed);
        }
        wasUnderrun_ = missing != 0;
        buffered_.store(ring_.end() - (started_ ? cursor_ : ring_.begin()), std::memory_order_relaxed);
    }
private:
    void seek(uint64_t cursor) {
        cursor_ = cursor; phase_ = 0; fade_ = 0; filteredDepth_ = double(target_); correction_ = 0;
    }
    void recordDrop(uint64_t frames) { metrics_.droppedNs.fetch_add(frames * 1000000000 / sourceRate_, std::memory_order_relaxed); }
    int sourceRate_, outputRate_;
    Metrics& metrics_;
    uint64_t target_;
    double ratio_;
    FrameRing<32768> ring_;
    SincResampler filter_;
    std::atomic<uint64_t> buffered_{0};
    std::atomic<int> correctionPpm_{0};
    std::atomic<uint64_t> resetBoundary_{UINT64_MAX};
    uint64_t handledBoundary_ = UINT64_MAX;
    uint64_t cursor_ = 0;
    double phase_ = 0, filteredDepth_ = 0, correction_ = 0;
    bool started_ = false, everStarted_ = false, wasUnderrun_ = false;
    int fade_ = 0;
    Stereo last_;
};
}
