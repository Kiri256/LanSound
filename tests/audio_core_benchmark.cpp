#include "../shared/audio_core.h"
#include "../shared/playback_buffer.h"
#include <chrono>
#include <iostream>
using namespace lanaudio;
using Clock = std::chrono::steady_clock;
int main() {
    constexpr int rounds = 500000;
    std::array<Stereo, 96> input{};
    for (size_t i = 0; i < input.size(); ++i) input[i] = {float(i), -float(i)};
    std::array<double, 5> timings{};
    double checksum = 0;
    for (auto& timing : timings) {
        FrameRing<32768> ring;
        const auto start = Clock::now();
        for (int i = 0; i < rounds; ++i) {
            if (!ring.write(input.data(), input.size())) return 1;
            const auto begin = ring.begin();
            checksum += ring.at(begin + i % input.size()).left;
            ring.release(begin + input.size());
        }
        timing = std::chrono::duration<double, std::nano>(Clock::now() - start).count() / rounds;
    }
    std::sort(timings.begin(), timings.end());
    std::cout << "96-frame ring write/read/release median ns=" << timings[2] << " checksum=" << checksum << '\n';
    FrameRing<16384> ring;
    std::array<Stereo, 16384> wave{};
    for (size_t i = 0; i < wave.size(); ++i) wave[i] = {float(std::sin(i * 0.1)), float(std::cos(i * 0.1))};
    ring.write(wave.data(), wave.size());
    SincResampler filter(1.00005);
    checksum = 0;
    for (auto& timing : timings) {
        const auto start = Clock::now();
        for (int i = 0; i < 48000; ++i) {
            const auto sample = filter.sample(ring, 100 + i % 16000, double(i % 1000) / 1000);
            checksum += sample.left + sample.right;
        }
        timing = std::chrono::duration<double, std::nano>(Clock::now() - start).count() / 48000;
    }
    std::sort(timings.begin(), timings.end());
    std::cout << "96-tap sinc median ns/output frame=" << timings[2] << " checksum=" << checksum << '\n';
    checksum = 0;
    for (auto& timing : timings) {
        ReorderBuffer<int> reorder;
        int output = 0;
        const auto start = Clock::now();
        for (int i = 0; i < rounds; ++i) {
            if (reorder.insert(uint32_t(i), i) != decltype(reorder)::Insert::Accepted || !reorder.pop(output) || reorder.hasFuture()) return 1;
            checksum += output;
        }
        timing = std::chrono::duration<double, std::nano>(Clock::now() - start).count() / rounds;
    }
    std::sort(timings.begin(), timings.end());
    std::cout << "reorder insert/pop/empty median ns=" << timings[2] << " checksum=" << checksum << '\n';
    checksum = 0;
    for (auto& timing : timings) {
        Metrics metrics;
        PlaybackBuffer playback(48000, 48000, metrics);
        std::array<Stereo, 4992> startup{};
        playback.write(startup.data(), startup.size());
        std::array<float, 192> output{};
        constexpr int callbacks = 20000;
        const auto start = Clock::now();
        for (int i = 0; i < callbacks; ++i) {
            playback.write(input.data(), input.size());
            playback.render(output.data(), 96);
            checksum += output[(i % 96) * 2];
        }
        if (metrics.underruns != 0 || playback.correctionPpm() != 0) return 1;
        timing = std::chrono::duration<double, std::nano>(Clock::now() - start).count() / callbacks;
    }
    std::sort(timings.begin(), timings.end());
    std::cout << "96-frame direct write/render median ns=" << timings[2] << " checksum=" << checksum << '\n';
}
