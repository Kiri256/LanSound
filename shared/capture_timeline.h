#pragma once
#include "audio_protocol.h"
#include <optional>

namespace lanaudio {
// Capture and pacing are owned by one thread. Slots carry absolute frame tags,
// so missing or expired samples cannot be mistaken for a later ring generation.
class CaptureTimeline {
public:
    static constexpr size_t Capacity = 32768;
    CaptureTimeline(uint32_t rate, uint64_t originUs, uint64_t waitUs)
        : rate_(rate), originUs_(originUs), waitUs_(waitUs) {
        tags_.fill(UINT64_MAX);
    }
    uint64_t frameAt(uint64_t timeUs) const {
        if (timeUs <= originUs_) return 0;
        const auto elapsed = timeUs - originUs_;
        return elapsed / 1000000 * rate_ + elapsed % 1000000 * rate_ / 1000000;
    }
    uint64_t deadlineUs() const {
        const auto end = position_ + PacketFrames;
        return originUs_ + waitUs_ + end / rate_ * 1000000 +
            (end % rate_ * 1000000 + rate_ - 1) / rate_;
    }
    void write(uint64_t timestamp, const Stereo* samples, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            const auto frame = timestamp + i;
            if (frame < position_) { ++lateFrames; continue; }
            if (frame - position_ >= Capacity) { ++overflowFrames; continue; }
            const auto slot = frame % Capacity;
            samples_[slot] = samples[i]; tags_[slot] = frame;
        }
    }
    void capture(uint64_t devicePosition, uint64_t captureUs, bool discontinuity, const Stereo* samples, size_t count) {
        if (captureUs < originUs_) {
            const auto trim = std::min<uint64_t>(count, ((originUs_ - captureUs) * rate_ + 999999) / 1000000);
            lateFrames += trim; devicePosition += trim; samples += trim; count -= size_t(trim);
            if (!count) return;
        }
        auto frame = frameAt(captureUs);
        if (nextCaptureFrame_) {
            const auto difference = frame > *nextCaptureFrame_ ? frame - *nextCaptureFrame_ : *nextCaptureFrame_ - frame;
            if (!discontinuity && devicePosition == nextDevicePosition_ && difference <= waitUs_ * rate_ / 1000000)
                frame = *nextCaptureFrame_;
            else ++resyncs;
        }
        write(frame, samples, count);
        nextCaptureFrame_ = frame + count; nextDevicePosition_ = devicePosition + count;
    }
    bool next(uint64_t nowUs, AudioPacket& packet) {
        if (nowUs < deadlineUs()) return false;
        const auto due = frameAt(nowUs - waitUs_) / PacketFrames * PacketFrames;
        const auto limit = std::max<uint64_t>(1, uint64_t(rate_) * 20 / 1000 / PacketFrames) * PacketFrames;
        if (due - position_ > limit) {
            skippedFrames += due - position_ - limit;
            position_ = due - limit;
        }
        packet.timestamp = position_;
        for (auto& sample : packet.samples) {
            const auto slot = position_ % Capacity;
            if (tags_[slot] == position_) sample = samples_[slot];
            else { sample = {}; ++silenceFrames; }
            ++position_;
        }
        return true;
    }
    uint64_t silenceFrames = 0, lateFrames = 0, overflowFrames = 0, skippedFrames = 0, resyncs = 0;
private:
    uint32_t rate_;
    uint64_t originUs_, waitUs_, position_ = 0;
    std::optional<uint64_t> nextCaptureFrame_;
    uint64_t nextDevicePosition_ = 0;
    std::array<Stereo, Capacity> samples_{};
    std::array<uint64_t, Capacity> tags_{};
};
}
