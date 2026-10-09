#pragma once
#include <winsock2.h>
#include <windows.h>
#include "../shared/audio_core.h"
#include "../shared/audio_protocol.h"
#include "../shared/audio_metrics.h"
#include <chrono>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace lanaudio {
struct Datagram { std::array<uint8_t, PacketBytes> bytes{}; uint64_t createdMs = 0; };
class PacketSender {
public:
    explicit PacketSender(SOCKET socket) : socket_(socket), worker_(&PacketSender::sendLoop, this) {}
    ~PacketSender() {
        stop_.store(true); SetEvent(ready_);
        if (worker_.joinable()) worker_.join();
        CloseHandle(ready_);
    }
    void setTarget(const sockaddr_in& target) {
        std::lock_guard<std::mutex> lock(targetMutex_);
        target_ = target; lastHelloMs_ = GetTickCount64();
    }
    // These methods have exactly one caller: the capture thread.
    void configure(uint32_t rate) {
        rate_ = rate;
        if (!session_) {
            LARGE_INTEGER counter; QueryPerformanceCounter(&counter);
            session_ = uint64_t(counter.QuadPart) ^ (uint64_t(GetCurrentProcessId()) << 32);
        } else ++session_;
        if (!session_) session_ = 1;
    }
    void write(const AudioPacket& audio) {
        const auto w = write_.load(std::memory_order_relaxed);
        const auto r = read_.load(std::memory_order_acquire);
        if (w - r < queue_.size()) {
            auto& packet = queue_[w % queue_.size()];
            auto* h = packet.bytes.data();
            put32(h, Magic); h[4] = Version; h[5] = 2;
            put16(h + 6, HeaderBytes); put32(h + 8, rate_);
            put16(h + 12, PacketFrames); put16(h + 14, 0);
            put32(h + 16, sequenceFromTimestamp(audio.timestamp)); put64(h + 20, session_);
            put64(h + 28, audio.timestamp); put32(h + 36, 0);
            std::memcpy(h + HeaderBytes, audio.samples.data(), sizeof(audio.samples));
            packet.createdMs = GetTickCount64();
            write_.store(w + 1, std::memory_order_release);
            SetEvent(ready_);
        } else queueDrops_.fetch_add(1, std::memory_order_relaxed);
    }
    std::string stats() const {
        return "send_queue_drops=" + std::to_string(queueDrops_.load()) +
            " expired_packets=" + std::to_string(expired_.load()) +
            " no_target_packets=" + std::to_string(noTarget_.load()) +
            " send_errors=" + std::to_string(sendErrors_.load()) +
            " sent_packets=" + std::to_string(sentPackets_.load()) +
            " send_max_gap_ms=" + std::to_string(sendIntervals_.maxUs.load() / 1000) +
            " send_long_gaps=" + std::to_string(sendIntervals_.longGaps.load());
    }
private:
    static HANDLE createReadyEvent() {
        auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) throw std::runtime_error("Cannot create sender event");
        return event;
    }
    void sendLoop() {
        using Clock = std::chrono::steady_clock;
        auto previousSend = Clock::time_point{};
        sockaddr_in previousTarget{};
        while (!stop_.load()) {
            auto r = read_.load(std::memory_order_relaxed);
            const auto w = write_.load(std::memory_order_acquire);
            if (r == w) { WaitForSingleObject(ready_, 100); continue; }
            // The producer cannot reuse this slot until read_ advances. Send
            // directly from the queue to avoid copying every 808-byte packet.
            const auto& packet = queue_[r % queue_.size()];
            sockaddr_in destination{};
            uint64_t nowMs;
            { std::lock_guard<std::mutex> lock(targetMutex_);
              nowMs = GetTickCount64();
              if (nowMs - lastHelloMs_ < 3000) destination = target_; }
            if (nowMs - packet.createdMs > 20) {
                expired_.fetch_add(1, std::memory_order_relaxed);
            } else if (destination.sin_family != AF_INET) {
                noTarget_.fetch_add(1, std::memory_order_relaxed);
                previousSend = {};
            } else if (sendto(socket_, reinterpret_cast<const char*>(packet.bytes.data()), int(packet.bytes.size()), 0,
                              reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)) == SOCKET_ERROR) {
                sendErrors_.fetch_add(1, std::memory_order_relaxed);
            } else {
                const auto now = Clock::now();
                if (previousSend != Clock::time_point{} && destination.sin_addr.s_addr == previousTarget.sin_addr.s_addr &&
                    destination.sin_port == previousTarget.sin_port)
                    sendIntervals_.record(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - previousSend).count()));
                previousSend = now; previousTarget = destination;
                sentPackets_.fetch_add(1, std::memory_order_relaxed);
            }
            read_.store(r + 1, std::memory_order_release);
        }
    }
    SOCKET socket_;
    sockaddr_in target_{};
    std::mutex targetMutex_;
    uint64_t lastHelloMs_ = 0;
    std::array<Datagram, 32> queue_{};
    alignas(64) std::atomic<uint64_t> write_{0}, read_{0};
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> queueDrops_{0}, expired_{0}, noTarget_{0}, sendErrors_{0};
    std::atomic<uint64_t> sentPackets_{0};
    IntervalStats sendIntervals_;
    uint32_t rate_ = 48000;
    uint64_t session_ = 0;
    HANDLE ready_ = createReadyEvent();
    // Must be last: all state is initialized before the worker starts.
    std::thread worker_;
};

}
