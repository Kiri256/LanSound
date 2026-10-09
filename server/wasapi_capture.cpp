#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <avrt.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include "../shared/capture_timeline.h"
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "packet_sender.h"
#include "wasapi_capture.h"
using Microsoft::WRL::ComPtr;
namespace lanaudio {
uint64_t clockUs() {
    static const uint64_t frequency = [] { LARGE_INTEGER value; QueryPerformanceFrequency(&value); return uint64_t(value.QuadPart); }();
    LARGE_INTEGER value; QueryPerformanceCounter(&value);
    const auto ticks = uint64_t(value.QuadPart);
    return ticks / frequency * 1000000 + ticks % frequency * 1000000 / frequency;
}
float readSample(const uint8_t* p, WORD bits, bool isFloat) {
    if (isFloat) { float v; std::memcpy(&v, p, sizeof(v)); return std::isfinite(v) ? v : 0; }
    if (bits == 16) { int16_t v; std::memcpy(&v, p, 2); return float(v) / 32768; }
    if (bits == 24) {
        int32_t v = int32_t(p[0]) | int32_t(p[1]) << 8 | int32_t(p[2]) << 16;
        if (v & 0x800000) v |= ~0xffffff;
        return float(v) / 8388608;
    }
    int32_t v; std::memcpy(&v, p, 4); return float(double(v) / 2147483648.0);
}
std::wstring deviceId(IMMDevice* device) {
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id))) throw std::runtime_error("Cannot read playback device ID");
    std::wstring result(id); CoTaskMemFree(id); return result;
}
struct CaptureResources {
    WAVEFORMATEX* mix = nullptr;
    HANDLE event = nullptr, timer = nullptr, task = nullptr;
    ComPtr<IAudioClient> client;
    ~CaptureResources() {
        if (client) client->Stop();
        if (mix) CoTaskMemFree(mix);
        if (event) CloseHandle(event);
        if (timer) CloseHandle(timer);
        if (task) AvRevertMmThreadCharacteristics(task);
    }
};
struct CaptureReport {
    CaptureTimeline& timeline;
    uint64_t timestampErrors = 0;
    void print() const {
        std::cout << "silence_frames=" << timeline.silenceFrames << " late_capture_frames=" << timeline.lateFrames
                  << " capture_overflow_frames=" << timeline.overflowFrames << " pacing_skipped_frames=" << timeline.skippedFrames
                  << " source_resyncs=" << timeline.resyncs << " timestamp_errors=" << timestampErrors << std::endl;
    }
    ~CaptureReport() { print(); }
};
void captureLoop(PacketSender& sender, const std::atomic<bool>& running) {
    CaptureResources resources;
    DWORD taskIndex = 0;
    resources.task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioCaptureClient> capture;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        throw std::runtime_error("Cannot create device enumerator");
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) throw std::runtime_error("No playback device");
    const auto initialDeviceId = deviceId(device.Get());
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &resources.client))) throw std::runtime_error("Cannot activate WASAPI");
    if (FAILED(resources.client->GetMixFormat(&resources.mix))) throw std::runtime_error("Cannot get mix format");
    auto* mix = resources.mix;
    bool isFloat = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    DWORD mask = 0;
    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        if (mix->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) throw std::runtime_error("Invalid extensible format");
        auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix);
        isFloat = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        if (!isFloat && !IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) throw std::runtime_error("Unsupported subtype");
        mask = ext->dwChannelMask;
    } else if (!isFloat && mix->wFormatTag != WAVE_FORMAT_PCM) throw std::runtime_error("Unsupported format");
    if (mix->nChannels == 0 || mix->nChannels > 8 || (mix->wBitsPerSample != 16 && mix->wBitsPerSample != 24 && mix->wBitsPerSample != 32) ||
        (isFloat && mix->wBitsPerSample != 32) || mix->nSamplesPerSec < 8000 || mix->nSamplesPerSec > 192000)
        throw std::runtime_error("Unsupported mix rate/channels/bits");
    const size_t bytesPerSample = mix->wBitsPerSample / 8;
    if (mix->nBlockAlign != bytesPerSample * mix->nChannels) throw std::runtime_error("Invalid block alignment");
    if (!mask) {
        if (mix->nChannels == 1) mask = SPEAKER_FRONT_CENTER;
        else if (mix->nChannels == 2) mask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        else throw std::runtime_error("Multichannel format has no channel mask");
    }
    std::array<Stereo, 8> weights{};
    size_t channel = 0;
    constexpr float surround = 0.70710678f;
    float leftSum = 0, rightSum = 0;
    for (unsigned bit = 0; bit < 32; ++bit) {
        const DWORD speaker = DWORD(1) << bit;
        if (!(mask & speaker)) continue;
        if (channel >= mix->nChannels) throw std::runtime_error("Channel mask mismatch");
        Stereo weight;
        switch (speaker) {
        case SPEAKER_FRONT_LEFT: weight.left = 1; break;
        case SPEAKER_FRONT_RIGHT: weight.right = 1; break;
        case SPEAKER_FRONT_CENTER: weight = {surround, surround}; break;
        case SPEAKER_BACK_LEFT: case SPEAKER_SIDE_LEFT: case SPEAKER_FRONT_LEFT_OF_CENTER: weight.left = surround; break;
        case SPEAKER_BACK_RIGHT: case SPEAKER_SIDE_RIGHT: case SPEAKER_FRONT_RIGHT_OF_CENTER: weight.right = surround; break;
        case SPEAKER_BACK_CENTER: weight = {0.5f, 0.5f}; break;
        case SPEAKER_LOW_FREQUENCY: break; // No bass management: omit LFE instead of overloading stereo.
        default: throw std::runtime_error("Unsupported speaker position");
        }
        weights[channel++] = weight; leftSum += weight.left; rightSum += weight.right;
    }
    if (channel != mix->nChannels) throw std::runtime_error("Channel mask mismatch");
    if (mix->nChannels == 1) weights[0] = {1,1};
    else if (mix->nChannels > 2) {
        const float gain = 1 / std::max({1.0f, leftSum, rightSum});
        for (auto& w : weights) { w.left *= gain; w.right *= gain; }
    }
    const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (FAILED(resources.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 100000, 0, mix, nullptr))) throw std::runtime_error("WASAPI initialization failed");
    resources.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!resources.event || FAILED(resources.client->SetEventHandle(resources.event))) throw std::runtime_error("Cannot set capture event");
    if (FAILED(resources.client->GetService(IID_PPV_ARGS(&capture)))) throw std::runtime_error("Cannot get capture service");
    UINT32 bufferFrames = 0;
    if (FAILED(resources.client->GetBufferSize(&bufferFrames))) throw std::runtime_error("Cannot get buffer size");
    std::vector<Stereo> converted(bufferFrames);
    REFERENCE_TIME devicePeriod = 0;
    if (FAILED(resources.client->GetDevicePeriod(&devicePeriod, nullptr))) throw std::runtime_error("Cannot get device period");
    const uint64_t waitUs = std::max<uint64_t>((devicePeriod + 9) / 10,
        (uint64_t(bufferFrames) * 1000000 + mix->nSamplesPerSec - 1) / mix->nSamplesPerSec) +
        (PacketFrames * 1000000 + mix->nSamplesPerSec - 1) / mix->nSamplesPerSec;
    constexpr DWORD highResolutionTimer = 0x2; // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, Windows 10 1803+.
    resources.timer = CreateWaitableTimerExW(nullptr, nullptr, highResolutionTimer, TIMER_ALL_ACCESS);
    const bool highResolution = resources.timer != nullptr;
    if (!resources.timer) resources.timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (!resources.timer) throw std::runtime_error("Cannot create pacing timer");
    auto timeline = std::make_unique<CaptureTimeline>(mix->nSamplesPerSec, clockUs(), waitUs);
    CaptureReport report{*timeline};
    sender.configure(mix->nSamplesPerSec);
    if (FAILED(resources.client->Start())) throw std::runtime_error("Cannot start capture");
    std::cout << "LanAudio server v" << LANAUDIO_VERSION_STRING << "; capturing "
              << mix->nSamplesPerSec << " Hz, " << mix->nChannels
              << " channels; protocol v" << unsigned(Version)
              << "; capture_wait_us=" << waitUs << "; high_resolution_timer=" << highResolution << std::endl;
    uint64_t lastDeviceCheck = GetTickCount64();
    uint64_t lastReport = GetTickCount64();
    while (running.load()) {
        if (GetTickCount64() - lastDeviceCheck > 2000) {
            ComPtr<IMMDevice> current;
            if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &current)) || deviceId(current.Get()) != initialDeviceId)
                throw std::runtime_error("Default playback device changed");
            lastDeviceCheck = GetTickCount64();
        }
        UINT32 packetFrames = 0;
        if (FAILED(capture->GetNextPacketSize(&packetFrames))) throw std::runtime_error("Capture device unavailable");
        for (int batches = 0; packetFrames && batches < 128; ++batches) {
            BYTE* data = nullptr; UINT32 frames = 0; DWORD bufferFlags = 0;
            UINT64 devicePosition = 0, qpcPosition = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &bufferFlags, &devicePosition, &qpcPosition))) throw std::runtime_error("Capture read failed");
            if (frames > converted.size()) { capture->ReleaseBuffer(frames); throw std::runtime_error("Unexpected capture buffer size"); }
            for (UINT32 f = 0; f < frames; ++f) {
                Stereo sample;
                if (!(bufferFlags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    const auto* row = data + size_t(f) * mix->nBlockAlign;
                    for (size_t c = 0; c < mix->nChannels; ++c) {
                        const auto value = readSample(row + c * bytesPerSample, mix->wBitsPerSample, isFloat);
                        sample.left += value * weights[c].left; sample.right += value * weights[c].right;
                    }
                }
                converted[f] = sample;
            }
            // Release WASAPI before packetization; socket I/O runs on another thread.
            if (FAILED(capture->ReleaseBuffer(frames))) throw std::runtime_error("Capture release failed");
            const bool timestampError = (bufferFlags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0;
            const auto capturedUs = timestampError ? clockUs() - uint64_t(frames) * 1000000 / mix->nSamplesPerSec : qpcPosition / 10;
            if (timestampError) ++report.timestampErrors;
            timeline->capture(devicePosition, capturedUs, (bufferFlags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0,
                converted.data(), frames);
            if (FAILED(capture->GetNextPacketSize(&packetFrames))) throw std::runtime_error("Capture device unavailable");
        }
        AudioPacket packet;
        const auto nowUs = clockUs();
        while (timeline->next(nowUs, packet)) sender.write(packet);
        if (GetTickCount64() - lastReport >= 5000) {
            report.print(); std::cout << sender.stats() << '\n'; lastReport = GetTickCount64();
        }
        LARGE_INTEGER due;
        const auto beforeWait = clockUs();
        due.QuadPart = -LONGLONG(std::max<uint64_t>(1, timeline->deadlineUs() > beforeWait ? timeline->deadlineUs() - beforeWait : 1) * 10);
        if (!SetWaitableTimer(resources.timer, &due, 0, nullptr, nullptr, FALSE)) throw std::runtime_error("Cannot arm pacing timer");
        const HANDLE events[] = {resources.event, resources.timer};
        const auto wait = WaitForMultipleObjects(2, events, FALSE, 100);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_OBJECT_0 + 1 && wait != WAIT_TIMEOUT) throw std::runtime_error("Capture wait failed");
    }
}

}
