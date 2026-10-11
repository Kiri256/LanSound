#pragma once
#include <oboe/Oboe.h>
#include <android/log.h>
#include "../../../../../shared/playback_buffer.h"
#include <string>
#include <memory>

namespace lanaudio {
// All stream lifecycle operations run on the receiver thread; the callback only
// renders the platform-independent buffer or requests recovery after an error.
class AudioPlayer final : public oboe::AudioStreamDataCallback, public oboe::AudioStreamErrorCallback {
public:
    AudioPlayer(int rate, Metrics& metrics, int targetMs) : rate_(rate), metrics_(metrics), targetMs_(targetMs) {}
    ~AudioPlayer() { close(); }
    bool open() {
        auto tryOpen = [&](oboe::SharingMode sharing) {
            oboe::AudioStreamBuilder builder;
            builder.setDirection(oboe::Direction::Output)->setFormat(oboe::AudioFormat::Float)
                ->setChannelCount(2)->setPerformanceMode(oboe::PerformanceMode::LowLatency)
                ->setSharingMode(sharing)->setUsage(oboe::Usage::Game)
                ->setContentType(oboe::ContentType::Music)->setDataCallback(this)->setErrorCallback(this);
            return builder.openStream(stream_);
        };
        auto result = tryOpen(oboe::SharingMode::Exclusive);
        if (result != oboe::Result::OK) result = tryOpen(oboe::SharingMode::Shared);
        if (result != oboe::Result::OK) return false;
        outputRate_ = stream_->getSampleRate();
        if (stream_->getFormat() != oboe::AudioFormat::Float || stream_->getChannelCount() != 2 || outputRate_ <= 0) {
            close(); return false;
        }
        buffer_ = std::make_unique<PlaybackBuffer>(rate_, outputRate_, metrics_, targetMs_);
        stream_->setBufferSizeInFrames(stream_->getFramesPerBurst() * 2);
        if (stream_->requestStart() != oboe::Result::OK) { close(); return false; }
        return true;
    }
    void close() {
        if (stream_) { stream_->requestStop(); stream_->close(); stream_.reset(); }
        buffer_.reset();
    }
    bool needsRestart() const { return restart_.load(std::memory_order_acquire); }
    int sourceRate() const { return rate_; }
    void resetTimeline() { buffer_->reset(); }
    void write(const Stereo* samples, size_t count) { buffer_->write(samples, count); }
    void requestTargetMs(int targetMs) { if (buffer_) buffer_->requestTargetMs(targetMs); }
    void silence() { buffer_->silence(); }
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream*, void* data, int32_t frames) override {
        buffer_->render(static_cast<float*>(data), frames);
        return oboe::DataCallbackResult::Continue;
    }
    void onErrorAfterClose(oboe::AudioStream*, oboe::Result error) override {
        __android_log_print(ANDROID_LOG_WARN, "LanAudio", "Output recovery: %s", oboe::convertToText(error));
        restart_.store(true, std::memory_order_release);
    }
    std::string stats() const {
        auto xruns = stream_->getXRunCount();
        return "buffer_ms=" + std::to_string(buffer_->bufferedMs()) +
            " target_ms=" + std::to_string(buffer_->targetMs()) +
            " correction_ppm=" + std::to_string(buffer_->correctionPpm()) +
            " output_hz=" + std::to_string(outputRate_) +
            " hal_xruns=" + std::to_string(xruns ? xruns.value() : -1) +
            " missing_ms=" + std::to_string(metrics_.missingNs.load() / 1000000) +
            " dropped_ms=" + std::to_string(metrics_.droppedNs.load() / 1000000);
    }
private:
    int rate_, outputRate_ = 0;
    int targetMs_;
    Metrics& metrics_;
    std::shared_ptr<oboe::AudioStream> stream_;
    std::unique_ptr<PlaybackBuffer> buffer_;
    std::atomic<bool> restart_{false};
};
}
