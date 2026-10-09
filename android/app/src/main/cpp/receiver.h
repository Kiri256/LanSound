#pragma once
#include "audio_player.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>

namespace lanaudio {
using Clock = std::chrono::steady_clock;

class Session {
public:
    Session(const char* host, int port) {
        server_.sin_family = AF_INET; server_.sin_port = htons(uint16_t(port));
        valid_ = inet_pton(AF_INET, host, &server_.sin_addr) == 1;
    }
    ~Session() { stop(); }
    bool start() {
        if (!valid_) return false;
        socket_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_ < 0) return false;
        int bytes = 256 * 1024;
        setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
        timeval timeout{0, 5000};
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        // Ephemeral local port; connected UDP filters other senders.
        if (::connect(socket_, reinterpret_cast<sockaddr*>(&server_), sizeof(server_)) < 0) { ::close(socket_); socket_ = -1; return false; }
        running_.store(true); receiver_ = std::thread(&Session::receive, this); return true;
    }
    void stop() {
        running_.store(false);
        if (socket_ >= 0) ::shutdown(socket_, SHUT_RDWR);
        // Always join even if receive already exited.
        if (receiver_.joinable()) receiver_.join();
        resetPlayer();
        if (socket_ >= 0) { ::close(socket_); socket_ = -1; }
    }
    std::string stats() const {
        std::lock_guard<std::mutex> lock(playerMutex_);
        std::string s = "packets=" + std::to_string(metrics_.packets.load()) +
            " lost_packets=" + std::to_string(metrics_.lost.load()) +
            " late_packets=" + std::to_string(metrics_.late.load()) +
            " duplicates=" + std::to_string(metrics_.duplicates.load()) +
            " max_gap_ms=" + std::to_string(metrics_.maxGapMs.load()) +
            " long_gaps=" + std::to_string(metrics_.longGaps.load()) +
            " queue_drops=" + std::to_string(metrics_.queueDrops.load()) +
            " active_skips=" + std::to_string(metrics_.activeSkips.load()) +
            " underruns=" + std::to_string(metrics_.underruns.load()) +
            " reconnects=" + std::to_string(metrics_.reconnects.load()) +
            " stalls=" + std::to_string(metrics_.stallEvents.load()) +
            "\nrecv_poll_max_ms=" + std::to_string(metrics_.receivePoll.maxUs.load() / 1000) +
            " recv_long_polls=" + std::to_string(metrics_.receivePoll.longGaps.load()) +
            "\nrecover_output=" + std::to_string(metrics_.outputRecoveries.load()) +
            " recover_sequence=" + std::to_string(metrics_.sequenceRecoveries.load()) +
            " recover_timestamp=" + std::to_string(metrics_.timestampRecoveries.load()) +
            " recover_session=" + std::to_string(metrics_.sourceSessionRecoveries.load()) +
            " recover_rate=" + std::to_string(metrics_.sourceRateRecoveries.load()) +
            " output_open_failures=" + std::to_string(metrics_.outputOpenFailures.load());
        return s + "\n" + (player_ ? player_->stats() : "等待服务器音频 / 正在恢复连接");
    }
private:
    void resetPlayer() { std::lock_guard<std::mutex> lock(playerMutex_); player_.reset(); }
    void receive() {
        ReorderBuffer<AudioPacket> reorder;
        std::array<uint8_t, PacketBytes + 1> bytes{};
        uint64_t session = 0;
        std::optional<uint64_t> nextTimestamp;
        std::array<uint64_t, 16> retiredSessions{};
        size_t retiredAt = 0;
        auto lastHello = Clock::now() - std::chrono::seconds(1);
        auto lastGoodAudio = Clock::now();
        auto gapSince = Clock::time_point{};
        auto previousArrival = Clock::time_point{};
        uint64_t previousTimestamp = 0;
        double jitterMs = 0;
        auto previousPoll = Clock::now();
        auto recover = [&](RecoveryCause cause, bool count = true) {
            if (cause == RecoveryCause::Output || cause == RecoveryCause::SourceRate) resetPlayer();
            else if (player_) player_->resetTimeline();
            reorder.reset(); nextTimestamp.reset(); gapSince = {};
            if (cause != RecoveryCause::Sequence) {
                session = 0; previousArrival = {}; jitterMs = 0;
                lastGoodAudio = Clock::now();
            }
            if (count) metrics_.recordRecovery(cause);
        };
        while (running_.load()) {
            auto now = Clock::now();
            if (now - lastHello >= std::chrono::milliseconds(500)) {
                const uint8_t hello[] = {'L','A','H','I',Version};
                ::send(socket_, hello, sizeof(hello), 0); lastHello = now;
            }
            const auto length = ::recv(socket_, bytes.data(), bytes.size(), 0);
            now = Clock::now();
            metrics_.receivePoll.record(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - previousPoll).count()));
            previousPoll = now;
            if (player_ && player_->needsRestart()) {
                recover(RecoveryCause::Output);
            } else if (length <= 0 && session && now - lastGoodAudio > std::chrono::seconds(1)) {
                recover(RecoveryCause::Stall);
            }
            PacketInfo info;
            if (length > 0 && parsePacket(bytes.data(), size_t(length), info)) {
                const auto incomingRate = info.rate;
                const auto incomingSession = info.session;
                const auto timestamp = info.timestamp;
                if (std::find(retiredSessions.begin(), retiredSessions.end(), incomingSession) != retiredSessions.end()) continue;
                const bool reopen = !player_ || player_->sourceRate() != int(incomingRate);
                if (session != incomingSession || reopen) {
                    const bool count = session != 0 || (player_ && reopen);
                    if (session && session != incomingSession) retiredSessions[retiredAt++ % retiredSessions.size()] = session;
                    recover(reopen ? RecoveryCause::SourceRate : RecoveryCause::SourceSession, count);
                    if (reopen) {
                        auto candidate = std::make_unique<AudioPlayer>(int(incomingRate), metrics_);
                        if (!candidate->open()) {
                            metrics_.outputOpenFailures.fetch_add(1, std::memory_order_relaxed);
                            std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue;
                        }
                        { std::lock_guard<std::mutex> lock(playerMutex_); player_ = std::move(candidate); }
                    }
                    session = incomingSession;
                }
                if (previousArrival == Clock::time_point{} || timestamp > previousTimestamp) {
                    if (previousArrival != Clock::time_point{}) {
                        const double arrival = std::chrono::duration<double, std::milli>(now - previousArrival).count();
                        const double source = double(timestamp - previousTimestamp) * 1000 / incomingRate;
                        jitterMs += (std::min(100.0, std::abs(arrival - source)) - jitterMs) / 16;
                        const auto gap = uint64_t(std::max(0.0, arrival));
                        auto old = metrics_.maxGapMs.load(std::memory_order_relaxed);
                        while (gap > old && !metrics_.maxGapMs.compare_exchange_weak(old, gap, std::memory_order_relaxed)) {}
                        if (gap >= 20) metrics_.longGaps.fetch_add(1, std::memory_order_relaxed);
                    }
                    previousArrival = now; previousTimestamp = timestamp;
                }
                const auto packet = decodePacket(bytes.data(), info);
                auto result = reorder.insert(info.sequence, packet);
                if (result == decltype(reorder)::Insert::TooFar) {
                    recover(RecoveryCause::Sequence);
                    result = reorder.insert(info.sequence, packet);
                }
                if (result == decltype(reorder)::Insert::Late) {
                    metrics_.late.fetch_add(1, std::memory_order_relaxed);
                } else if (result == decltype(reorder)::Insert::Duplicate) {
                    metrics_.duplicates.fetch_add(1, std::memory_order_relaxed);
                } else {
                    metrics_.packets.fetch_add(1, std::memory_order_relaxed);
                    lastGoodAudio = now;
                }
            }
            if (!player_) continue;
            const int waitMs = std::clamp(int(4 + 4 * jitterMs), 4, 20);
            AudioPacket next;
            for (int steps = 0; steps < 128; ++steps) {
                if (reorder.pop(next)) {
                    if (nextTimestamp && next.timestamp != *nextTimestamp) { recover(RecoveryCause::Timestamp); break; }
                    player_->write(next.samples.data(), PacketFrames);
                    nextTimestamp = next.timestamp + PacketFrames; gapSince = {};
                } else if (reorder.hasFuture()) {
                    if (gapSince == Clock::time_point{}) gapSince = now;
                    if (now - gapSince < std::chrono::milliseconds(waitMs)) break;
                    reorder.skip(); metrics_.lost.fetch_add(1); player_->silence();
                    if (nextTimestamp) *nextTimestamp += PacketFrames;
                } else { gapSince = {}; break; }
            }
        }
    }
    sockaddr_in server_{};
    bool valid_ = false;
    int socket_ = -1;
    std::atomic<bool> running_{false};
    std::thread receiver_;
    Metrics metrics_;
    std::unique_ptr<AudioPlayer> player_;
    mutable std::mutex playerMutex_;
};

}
