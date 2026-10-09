#include "../shared/playback_buffer.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>
using namespace lanaudio;
constexpr size_t StartupFrames = 48000 * PlaybackBuffer::TargetMs / 1000 + 192;
void check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
void playbackTest() {
    Metrics metrics;
    PlaybackBuffer buffer(48000, 48000, metrics);
    std::vector<Stereo> startup(StartupFrames, {0.25f, -0.25f});
    buffer.write(startup.data(), startup.size());
    std::array<float, 192> out{};
    for (int batch = 0; batch < 10000; ++batch) {
        std::array<Stereo, 96> input{};
        for (int i = 0; i < 96; ++i) {
            const auto value = i % 2 ? 1e-10f : float((batch * 96 + i) % 1000) / 2000;
            input[i] = {value, -value};
        }
        buffer.write(input.data(), input.size());
        buffer.render(out.data(), 96);
        for (const auto value : out) check(std::isfinite(value), "finite direct output");
        constexpr int lag = PlaybackBuffer::TargetMs / 2 - 1;
        if (batch > lag) {
            for (int i = 0; i < 96; ++i) {
                const auto expected = i % 2 ? 1e-10f : float(((batch - lag) * 96 + i) % 1000) / 2000;
                check(out[i * 2] == expected && out[i * 2 + 1] == -expected, "direct playback preserves every sample");
            }
        }
    }
    check(metrics.activeSkips == 0 && metrics.underruns == 0, "stable source never skips or underruns");
    check(buffer.bufferedMs() == PlaybackBuffer::TargetMs - 2 && buffer.correctionPpm() == 0, "stable clocks use sample-exact bypass at normal depth");
    for (int i = 0; i < PlaybackBuffer::TargetMs; ++i) buffer.render(out.data(), 96);
    check(metrics.underruns == 1 && metrics.missingNs > 0, "one continuous starvation event");
    buffer.write(startup.data(), startup.size());
    buffer.render(out.data(), 96);
    check(out.back() != 0, "refill resumes output");
    std::vector<Stereo> excess(4800, {0.1f, 0.1f});
    buffer.write(excess.data(), excess.size());
    buffer.render(out.data(), 96);
    check(metrics.activeSkips == 1, "bounded backlog is skipped once");
    buffer.reset();
    buffer.render(out.data(), 96);
    check(buffer.bufferedMs() == 0, "timeline reset releases stale samples");
    buffer.write(startup.data(), startup.size());
    buffer.render(out.data(), 96);
    check(buffer.bufferedMs() > 0, "reset can refill without device reopening");
}
void protocolTest() {
    std::array<uint8_t, PacketBytes> bytes{};
    put32(bytes.data(), Magic); bytes[4] = Version; bytes[5] = 2;
    put16(bytes.data() + 6, HeaderBytes); put32(bytes.data() + 8, 48000);
    put16(bytes.data() + 12, PacketFrames); put32(bytes.data() + 16, 42);
    put64(bytes.data() + 20, 123); put64(bytes.data() + 28, 42 * PacketFrames);
    PacketInfo info;
    check(parsePacket(bytes.data(), bytes.size(), info) && info.sequence == 42, "valid v2 packet");
    check(!parsePacket(bytes.data(), 4, info), "truncated packet rejected");
    put64(bytes.data() + 28, 43 * PacketFrames);
    check(!parsePacket(bytes.data(), bytes.size(), info), "inconsistent timestamp rejected");
    for (const uint64_t sequence : {0xffffffffULL, 0x100000000ULL, 0x100000001ULL}) {
        const auto timestamp = sequence * PacketFrames;
        put32(bytes.data() + 16, sequenceFromTimestamp(timestamp));
        put64(bytes.data() + 28, timestamp);
        check(parsePacket(bytes.data(), bytes.size(), info) && info.sequence == uint32_t(sequence), "timestamp-derived sequence wraps without resetting timeline");
    }
    put32(bytes.data() + 16, 42);
    put64(bytes.data() + 28, 42 * PacketFrames);
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(bytes.data() + HeaderBytes, &nan, sizeof(nan));
    check(parsePacket(bytes.data(), bytes.size(), info), "header remains valid");
    check(decodePacket(bytes.data(), info).samples[0].left == 0, "nonfinite PCM sanitized");
}
void rateConversionTest() {
    for (const int rate : {44100, 96000, 192000}) {
        Metrics metrics;
        PlaybackBuffer buffer(rate, 48000, metrics);
        std::vector<Stereo> startup(rate * (PlaybackBuffer::TargetMs + 20) / 1000, {0.1f, -0.1f});
        buffer.write(startup.data(), startup.size());
        std::array<float, 960> output{};
        for (int ms = 0; ms < 1000; ms += 10) {
            std::vector<Stereo> source(rate / 100, {0.1f, -0.1f});
            buffer.write(source.data(), source.size()); buffer.render(output.data(), 480);
            for (float v : output) check(std::isfinite(v), "rate conversion produces finite output");
            check(std::abs(output.back() + 0.1f) < 0.001f, "rate conversion preserves DC gain");
        }
        check(metrics.underruns == 0 && metrics.activeSkips == 0, "stable rate conversion never resets playback");
    }
}
void recoveryTest() {
    Metrics metrics;
    PlaybackBuffer buffer(48000, 48000, metrics);
    ReorderBuffer<AudioPacket> reorder;
    AudioPacket packet;
    packet.samples.fill({0.25f, -0.25f});
    check(reorder.insert(1, packet) == decltype(reorder)::Insert::Accepted, "old session packet queued");
    check(reorder.insert(1000, packet) == decltype(reorder)::Insert::TooFar, "large sequence gap requires recovery");
    std::vector<Stereo> startup(StartupFrames, {0.25f, -0.25f});
    std::array<float, 192> output{};
    buffer.write(startup.data(), startup.size());
    buffer.render(output.data(), 96);
    for (int cycle = 0; cycle < 10; ++cycle) {
        reorder.reset(); buffer.reset();
        buffer.render(output.data(), 96);
        check(!reorder.hasFuture() && buffer.bufferedMs() == 0, "recovery clears stale packet and audio queues");
        const uint32_t sequence = 1000 + cycle * 1000;
        check(reorder.insert(sequence, packet) == decltype(reorder)::Insert::Accepted, "new timeline accepts distant sequence");
        AudioPacket next;
        check(reorder.pop(next), "new timeline packet delivered");
        buffer.write(next.samples.data(), next.samples.size());
        buffer.render(output.data(), 96);
        check(buffer.bufferedMs() == 2, "recovery waits for prebuffer instead of playing partial refill");
        buffer.write(startup.data(), startup.size());
        buffer.render(output.data(), 96);
        buffer.render(output.data(), 96);
        check(output.back() == -0.25f, "repeated timeline recovery resumes valid audio");
    }
    // Device recreation may change the actual output rate while metrics persist.
    std::vector<Stereo> refill(StartupFrames + 768, {0.25f, -0.25f});
    for (const int outputRate : {44100, 48000}) {
        PlaybackBuffer replacement(48000, outputRate, metrics);
        replacement.write(refill.data(), refill.size());
        replacement.render(output.data(), 96);
        replacement.render(output.data(), 96);
        check(std::abs(output.back() + 0.25f) < 0.001f, "recreated output resumes with direct or resampled audio");
    }
}
void resetBoundaryTest() {
    for (int outputRate : {48000, 44100}) {
        Metrics metrics;
        PlaybackBuffer buffer(48000, outputRate, metrics);
        std::array<float, 192> output{};
        std::vector<Stereo> old(StartupFrames + 768, {0.8f, -0.8f});
        std::vector<Stereo> fresh(StartupFrames + 768, {0.2f, -0.2f});
        buffer.write(old.data(), old.size());
        buffer.reset();
        buffer.write(fresh.data(), fresh.size());
        buffer.render(output.data(), 96);
        buffer.render(output.data(), 96);
        check(std::abs(output.back() + 0.2f) < 0.001f, "reset preserves audio written after its boundary");
        buffer.reset(); buffer.write(old.data(), old.size());
        buffer.reset(); buffer.write(fresh.data(), fresh.size());
        buffer.render(output.data(), 96); buffer.render(output.data(), 96);
        check(std::abs(output.back() + 0.2f) < 0.001f, "multiple recovery requests retain only latest fresh timeline");
        // Repeated recovery must keep the reported queue depth bounded.
        buffer.reset();
        buffer.write(fresh.data(), fresh.size());
        for (int i = 0; i < 4; ++i) buffer.render(output.data(), 96);
        check(buffer.bufferedMs() < PlaybackBuffer::TargetMs + 20, "recovery never moves the consumer backward");
    }
}
void clockCompensationTest() {
    for (const double drift : {-0.0005, 0.0005}) {
        Metrics metrics;
        PlaybackBuffer buffer(48000, 48000, metrics);
        std::vector<Stereo> startup(StartupFrames, {0.2f, -0.2f});
        buffer.write(startup.data(), startup.size());
        std::array<float, 192> output{};
        buffer.render(output.data(), 96);
        double pending = 0;
        std::array<Stereo, 98> source;
        source.fill({0.2f, -0.2f});
        for (int batch = 0; batch < 60000; ++batch) {
            pending += 96 * (1 + drift);
            const auto frames = size_t(pending); pending -= frames;
            buffer.write(source.data(), frames); buffer.render(output.data(), 96);
            check(std::abs(output.back() + 0.2f) < 0.001f, "clock compensation preserves continuous DC output");
            check(std::abs(buffer.correctionPpm()) <= 2000, "clock compensation is bounded");
        }
        check(metrics.underruns == 0 && metrics.activeSkips == 0, "two minutes of clock drift needs no starvation or hard skips");
        check(buffer.bufferedMs() >= PlaybackBuffer::TargetMs - 20 && buffer.bufferedMs() <= PlaybackBuffer::TargetMs + 20, "clock drift retains bounded normal playback depth");
        check(buffer.correctionPpm() * drift > 0, "clock compensation follows the source drift direction");
    }
}
void continuityTest() {
    Metrics metrics;
    PlaybackBuffer buffer(48000, 48000, metrics);
    std::vector<Stereo> startup(StartupFrames, {0.2f, -0.2f});
    std::array<float, 192> output{};
    std::array<Stereo, 96> packet;
    packet.fill({0.2f, -0.2f});
    buffer.write(startup.data(), startup.size()); buffer.render(output.data(), 96);
    for (int batch = 0; batch < 10000; ++batch) {
        // A 30ms receive pause, followed by the complete delayed batch.
        if (batch % 500 == 15) {
            for (int i = 0; i < 16; ++i) buffer.write(packet.data(), packet.size());
        } else if (batch % 500 > 15) buffer.write(packet.data(), packet.size());
        buffer.render(output.data(), 96);
        check(std::abs(output.back() + 0.2f) < 0.001f, "short batching remains audible without a reset");
    }
    check(metrics.underruns == 0 && metrics.activeSkips == 0, "short receive pauses need neither starvation nor hard skips");
    std::vector<Stereo> backlog(1920, {0.2f, -0.2f});
    buffer.write(backlog.data(), backlog.size()); buffer.render(output.data(), 96);
    check(metrics.activeSkips == 0, "moderate backlog is not abruptly skipped");
    for (int batch = 0; batch < 30000; ++batch) {
        buffer.write(packet.data(), packet.size()); buffer.render(output.data(), 96);
    }
    check(buffer.bufferedMs() < PlaybackBuffer::TargetMs + 20 && metrics.activeSkips == 0, "moderate backlog drains smoothly toward normal depth");
    backlog.resize(9600);
    buffer.write(backlog.data(), backlog.size()); buffer.render(output.data(), 96);
    check(metrics.activeSkips == 1 && buffer.bufferedMs() < PlaybackBuffer::TargetMs + 20, "severe backlog still has an explicit latency bound");
}
void compensatedToneTest() {
    Metrics metrics;
    PlaybackBuffer buffer(48000, 48000, metrics);
    uint64_t sourceFrame = 0;
    auto sample = [&] {
        const float value = float(0.3 * std::sin(2 * 3.14159265358979323846 * 1000 * sourceFrame++ / 48000));
        return Stereo{value, -value};
    };
    std::vector<Stereo> startup(StartupFrames);
    for (auto& value : startup) value = sample();
    buffer.write(startup.data(), startup.size());
    std::array<float, 192> output{};
    buffer.render(output.data(), 96);
    std::array<Stereo, 98> source;
    double pending = 0, energy = 0;
    uint64_t measured = 0;
    float previous = output[190];
    for (int batch = 0; batch < 10000; ++batch) {
        pending += 96 * 1.0005;
        const auto frames = size_t(pending); pending -= frames;
        for (size_t i = 0; i < frames; ++i) source[i] = sample();
        buffer.write(source.data(), frames); buffer.render(output.data(), 96);
        for (size_t i = 0; i < output.size(); i += 2) {
            if (batch > 200) {
                check(std::abs(output[i] - previous) < 0.05f, "clock compensation introduces no discontinuity in a continuous tone");
                check(std::abs(output[i] + output[i + 1]) < 1e-6f, "compensation preserves independent stereo channels");
                energy += double(output[i]) * output[i]; ++measured;
            }
            previous = output[i];
        }
    }
    check(std::abs(std::sqrt(energy / measured) - 0.3 / std::sqrt(2.0)) < 0.002, "clock-compensated tone preserves RMS gain");
    check(metrics.activeSkips == 0 && metrics.underruns == 0, "clock-compensated tone needs no skip or refill");
}
void bufferAllowanceTest() {
    Metrics metrics;
    PlaybackBuffer buffer(48000, 48000, metrics);
    std::vector<Stereo> startup(StartupFrames, {0.2f, -0.2f});
    std::array<float, 192> output{};
    buffer.write(startup.data(), startup.size()); buffer.render(output.data(), 96);
    for (int i = 0; i < 40; ++i) {
        buffer.render(output.data(), 96);
        check(std::abs(output.back() + 0.2f) < 0.001f, "hundred millisecond target bridges an eighty millisecond receive pause");
    }
    std::vector<Stereo> delayed(3840, {0.2f, -0.2f});
    buffer.write(delayed.data(), delayed.size()); buffer.render(output.data(), 96);
    check(metrics.underruns == 0 && metrics.activeSkips == 0, "delayed batch resumes without starvation or hard skip");
}
int main() { playbackTest(); protocolTest(); rateConversionTest(); recoveryTest(); resetBoundaryTest(); clockCompensationTest(); continuityTest(); compensatedToneTest(); bufferAllowanceTest(); std::cout << "Playback, timeline and protocol tests passed\n"; }
