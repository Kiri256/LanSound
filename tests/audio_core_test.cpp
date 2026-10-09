#include "../shared/audio_core.h"
#include "../shared/audio_protocol.h"
#include "../shared/audio_metrics.h"
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

using namespace lanaudio;
void require(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
void ringTest() {
    FrameRing<8> ring;
    Stereo samples[8];
    for (int i = 0; i < 8; ++i) samples[i] = {float(i), -float(i)};
    require(ring.write(samples, 8), "exact capacity allowed");
    require(!ring.write(samples, 1), "full ring rejects write");
    require(ring.begin() == 0 && ring.at(0).left == 0, "producer never moves consumer or overwrites data");
    ring.release(5);
    require(ring.write(samples, 5), "wrapped write accepted");
    require(ring.at(7).left == 7 && ring.at(8).left == 0, "retained samples survive wrap");
    for (size_t i = 0; i < 5; ++i) require(ring.at(8 + i).left == float(i), "wrapped bulk write retains every frame");
    require(ring.write(nullptr, 0), "empty write does not access input");
    require(!ring.write(samples, 9), "oversized write rejected before input access");
    require(ring.contiguous(6, 3) == nullptr && ring.contiguous(8, 4)[3].left == 3, "contiguous view respects physical wrap");

    FrameRing<1024> concurrent;
    constexpr uint64_t count = 1000000;
    std::thread producer([&] {
        for (uint64_t i = 0; i < count; ++i) {
            Stereo s{float(i), -float(i)};
            while (!concurrent.write(&s, 1)) std::this_thread::yield();
        }
    });
    for (uint64_t i = 0; i < count; ++i) {
        while (concurrent.end() <= i) std::this_thread::yield();
        const auto s = concurrent.at(i);
        require(s.left == float(i) && s.right == -float(i), "concurrent FIFO retains sample ordering");
        concurrent.release(i + 1);
    }
    producer.join();
}
void reorderTest() {
    ReorderBuffer<int, 8> reorder;
    using R = decltype(reorder);
    int p = 0;
    require(reorder.insert(100, 100) == R::Insert::Accepted && reorder.pop(p) && p == 100, "first packet");
    require(reorder.insert(102, 102) == R::Insert::Accepted && !reorder.pop(p), "gap waits instead of dropping reordered packet");
    require(reorder.insert(102, 102) == R::Insert::Duplicate, "duplicate detection");
    require(reorder.insert(101, 101) == R::Insert::Accepted && reorder.pop(p) && p == 101, "late arrival within deadline");
    require(reorder.pop(p) && p == 102, "future packet follows recovered gap");
    require(!reorder.hasFuture(), "delivering last packet clears pending count including duplicates");
    require(reorder.insert(101, 101) == R::Insert::Late, "expired packet rejected");
    require(reorder.insert(104, 104) == R::Insert::Accepted && reorder.hasFuture(), "future packet waits");
    reorder.skip();
    require(reorder.pop(p) && p == 104, "deadline skip resumes sequence");
    require(reorder.insert(120, 120) == R::Insert::TooFar, "large gap bounds memory/work");
    require(!reorder.hasFuture(), "late and too-far packets do not add pending entries");
    require(reorder.insert(106, 106) == R::Insert::Accepted && reorder.hasFuture(), "pending packet before reset");
    reorder.reset();
    require(!reorder.hasFuture(), "reset clears pending count with queued packets");
    require(reorder.insert(0xfffffffe, 1) == R::Insert::Accepted && reorder.pop(p), "wrap first");
    require(reorder.insert(0, 3) == R::Insert::Accepted, "wrap reordered future");
    require(reorder.insert(0xffffffff, 2) == R::Insert::Accepted && reorder.pop(p) && p == 2, "wrap gap recovered");
    require(reorder.pop(p) && p == 3, "wrap continuous delivery");
    require(!reorder.hasFuture(), "sequence wrap clears pending count");
}
double resampleRms(double ratio, double sourceFrequency) {
    FrameRing<16384> ring;
    std::vector<Stereo> input(16000);
    constexpr double pi = 3.14159265358979323846;
    for (size_t i = 0; i < input.size(); ++i) {
        const float value = float(std::sin(2 * pi * sourceFrequency * i));
        input[i] = {value, -value};
    }
    require(ring.write(input.data(), input.size()), "resampler input");
    SincResampler filter(ratio);
    double sum = 0, position = 100.25;
    constexpr int count = 2000;
    for (int i = 0; i < count; ++i, position += ratio) {
        const auto center = uint64_t(position);
        const auto out = filter.sample(ring, center, position - center);
        require(std::isfinite(out.left) && std::abs(out.left + out.right) < 1e-6, "finite, independent stereo channels");
        sum += double(out.left) * out.left;
    }
    return std::sqrt(sum / count);
}
void resamplerTest() {
    FrameRing<256> wrapped;
    FrameRing<1024> linear;
    std::vector<Stereo> input(456);
    for (size_t i = 0; i < input.size(); ++i) input[i] = {float(std::sin(i * 0.17)), float(std::cos(i * 0.29))};
    require(linear.write(input.data(), input.size()), "linear wrap reference");
    require(wrapped.write(input.data(), 256), "wrap reference first block");
    wrapped.release(200);
    require(wrapped.write(input.data() + 256, 200), "wrap reference second block");
    SincResampler wrapFilter(1.00005);
    for (uint64_t center = 248; center < 304; ++center) {
        for (double phase : {0.0, 0.12345, 0.5, 0.9999}) {
            const auto a = wrapFilter.sample(wrapped, center, phase);
            const auto b = wrapFilter.sample(linear, center, phase);
            require(std::abs(a.left - b.left) < 1e-6 && std::abs(a.right - b.right) < 1e-6,
                "resampling across physical wrap matches contiguous reference");
        }
    }
    FrameRing<128> dc;
    std::array<Stereo, 128> ones;
    ones.fill({0.25f, -0.5f});
    require(dc.write(ones.data(), ones.size()), "DC input");
    SincResampler filter(1);
    for (int p = 0; p < 256; ++p) {
        const auto s = filter.sample(dc, 64, double(p) / 256);
        require(std::abs(s.left - 0.25) < 1e-6 && std::abs(s.right + 0.5) < 1e-6, "unity DC at every phase");
    }
    const auto pass = resampleRms(1, 1000.0 / 48000);
    require(std::abs(pass - std::sqrt(0.5)) < 0.002, "1 kHz gain preserved");
    require(std::abs(resampleRms(44100.0 / 48000, 1000.0 / 44100) - std::sqrt(0.5)) < 0.002, "44.1 to 48 kHz");
    require(std::abs(resampleRms(1.0002, 18000.0 / 48000) - std::sqrt(0.5)) < 0.004, "18 kHz gain with clock drift");
    const auto alias = resampleRms(2, 30000.0 / 96000);
    require(alias < 0.002, "96 to 48 kHz rejects above-Nyquist tone");
    require(resampleRms(4, 30000.0 / 192000) < 0.002, "192 to 48 kHz rejects above-Nyquist tone");
    std::cout << "Resampler: 1kHz RMS=" << pass << ", downsample alias RMS=" << alias << '\n';
}
int main() {
    IntervalStats intervals;
    intervals.record(19999); intervals.record(20000); intervals.record(464000);
    require(intervals.longGaps == 2 && intervals.maxUs == 464000, "gap diagnostics preserve peak and inclusive twenty millisecond threshold");
    Metrics metrics;
    for (auto cause : {RecoveryCause::Output, RecoveryCause::Stall, RecoveryCause::Sequence,
                       RecoveryCause::Timestamp, RecoveryCause::SourceSession, RecoveryCause::SourceRate}) metrics.recordRecovery(cause);
    require(metrics.reconnects == 6 && metrics.stallEvents == 1 && metrics.outputRecoveries == 1 &&
        metrics.sequenceRecoveries == 1 && metrics.timestampRecoveries == 1 && metrics.sourceSessionRecoveries == 1 &&
        metrics.sourceRateRecoveries == 1, "recovery totals equal independently classified events");
    uint8_t header[40]{};
    put64(header + 20, 0x123456789abcdef0ULL);
    require(read64(header + 20) == 0x123456789abcdef0ULL, "protocol uint64 roundtrip");
    require(PacketBytes < 1200, "packet fits conservative UDP MTU");
    ringTest(); reorderTest(); resamplerTest();
    std::cout << "All audio core tests passed\n";
}
