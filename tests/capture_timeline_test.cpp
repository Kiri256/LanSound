#include "../shared/capture_timeline.h"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>
using namespace lanaudio;
void check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
void cadenceTest() {
    for (uint32_t rate : {44100u, 48000u, 96000u, 192000u}) {
        auto timeline = std::make_unique<CaptureTimeline>(rate, 1000000, 12000);
        AudioPacket packet;
        uint64_t packets = 0;
        for (uint64_t us = 1000000; us <= 2012000; us += 1000) {
            while (timeline->next(us, packet)) {
                check(packet.timestamp == packets * PacketFrames, "silence timeline is continuous at every rate");
                for (auto sample : packet.samples) check(sample.left == 0 && sample.right == 0, "empty source sends silence");
                ++packets;
            }
        }
        check(packets == rate / PacketFrames && timeline->skippedFrames == 0, "one second of source per second without capture");
        check(timeline->silenceFrames == packets * PacketFrames, "silence frames counted separately");
    }
}
void burstTest() {
    auto timeline = std::make_unique<CaptureTimeline>(48000, 0, 12000);
    AudioPacket packet;
    uint64_t expected = 0;
    for (uint64_t us = 0; us <= 200000; us += 1000) {
        if (us && us % 10000 == 0) {
            std::array<Stereo, 480> samples;
            const auto first = (us - 10000) * 48000 / 1000000;
            for (size_t i = 0; i < samples.size(); ++i) samples[i] = {float(first + i), -float(first + i)};
            timeline->capture(first, us - 10000, false, samples.data(), samples.size());
        }
        while (timeline->next(us, packet)) {
            check(packet.timestamp == expected, "batched capture does not inflate timeline");
            for (auto sample : packet.samples) {
                check(sample.left == float(expected) && sample.right == -float(expected), "batched PCM delivered exactly once");
                ++expected;
            }
        }
    }
    check(timeline->silenceFrames == 0 && timeline->lateFrames == 0, "normal capture bursts never receive inserted silence");
}
void expiryTest() {
    auto timeline = std::make_unique<CaptureTimeline>(48000, 0, 12000);
    AudioPacket packet;
    check(!timeline->next(13999, packet) && timeline->next(14000, packet), "packet waits until complete interval plus capture allowance");
    std::array<Stereo, 192> audio;
    audio.fill({0.5f, -0.5f});
    timeline->write(0, audio.data(), audio.size());
    check(timeline->lateFrames == 96, "late prefix trimmed after silence replacement");
    check(timeline->next(16000, packet) && packet.samples[0].left == 0.5f, "unexpired suffix retained");
    timeline->write(1008, audio.data(), 48);
    int catchup = 0;
    while (timeline->next(112000, packet)) ++catchup;
    check(catchup == 10 && timeline->skippedFrames == 3648, "long stall catches up at most twenty milliseconds");
    const auto silent = timeline->silenceFrames;
    timeline->write(4800, audio.data(), 48);
    check(timeline->next(114000, packet) && packet.samples[0].left == 0.5f && packet.samples[48].left == 0, "partial capture produces correctly positioned partial silence");
    check(timeline->silenceFrames - silent == 48, "partial silence counted accurately");
    timeline->write(100000, audio.data(), audio.size());
    check(timeline->overflowFrames == audio.size(), "future capacity overflow cannot overwrite valid slots");
}
void wrapTest() {
    auto timeline = std::make_unique<CaptureTimeline>(48000, 0, 12000);
    std::array<Stereo, 96> input;
    AudioPacket packet;
    for (uint64_t first = 0; first < 100000; first += 96) {
        input.fill({float(first), -float(first)});
        timeline->write(first, input.data(), input.size());
        check(timeline->next(timeline->deadlineUs(), packet) && packet.samples[0].left == float(first), "frame tags preserve samples across storage wrap");
    }
    check(timeline->silenceFrames == 0 && timeline->overflowFrames == 0, "storage wrap does not lose audio");
}
void positioningTest() {
    auto timeline = std::make_unique<CaptureTimeline>(48000, 1000000, 12000);
    std::array<Stereo, 96> input;
    input.fill({0.5f, -0.5f});
    AudioPacket packet;
    timeline->capture(100, 1000000, false, input.data(), input.size());
    timeline->capture(196, 1002001, false, input.data(), input.size());
    check(timeline->resyncs == 0, "QPC rounding does not break consecutive device samples");
    while (timeline->next(1112000, packet)) {}
    const auto silence = timeline->silenceFrames;
    timeline->capture(292, 1100000, false, input.data(), input.size());
    check(timeline->resyncs == 1, "idle resume uses QPC even when device frame position is contiguous");
    check(timeline->next(1114000, packet) && packet.timestamp == 4800 && packet.samples[0].left == 0.5f,
        "idle resume replaces only current source interval");
    check(timeline->silenceFrames == silence, "idle resume does not append silence to captured interval");
    timeline->capture(0, 1104000, true, input.data(), input.size());
    check(timeline->resyncs == 2, "device discontinuity reanchors source positions");
    auto startup = std::make_unique<CaptureTimeline>(48000, 1000000, 12000);
    startup->capture(0, 999000, false, input.data(), input.size());
    check(startup->next(1014000, packet) && startup->lateFrames == 48 && packet.samples[0].left == 0.5f &&
        packet.samples[48].left == 0, "pre-session capture prefix trimmed without shifting source timeline");
}
int main() { cadenceTest(); burstTest(); expiryTest(); wrapTest(); positioningTest(); std::cout << "Capture timeline tests passed\n"; }
