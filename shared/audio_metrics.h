#pragma once
#include <atomic>
#include <cstdint>

namespace lanaudio {
struct IntervalStats {
    void record(uint64_t us) {
        auto old = maxUs.load(std::memory_order_relaxed);
        while (us > old && !maxUs.compare_exchange_weak(old, us, std::memory_order_relaxed)) {}
        if (us >= 20000) longGaps.fetch_add(1, std::memory_order_relaxed);
    }
    std::atomic<uint64_t> maxUs{0}, longGaps{0};
};
enum class RecoveryCause { Output, Stall, Sequence, Timestamp, SourceSession, SourceRate };
struct Metrics {
    std::atomic<uint64_t> packets{0}, lost{0}, late{0}, duplicates{0};
    std::atomic<uint64_t> underruns{0}, missingNs{0}, droppedNs{0}, reconnects{0};
    std::atomic<uint64_t> queueDrops{0}, stallEvents{0}, longGaps{0}, activeSkips{0}, maxGapMs{0};
    IntervalStats receivePoll;
    std::atomic<uint64_t> outputRecoveries{0}, sequenceRecoveries{0}, timestampRecoveries{0};
    std::atomic<uint64_t> sourceSessionRecoveries{0}, sourceRateRecoveries{0}, outputOpenFailures{0};
    void recordRecovery(RecoveryCause cause) {
        switch (cause) {
        case RecoveryCause::Output: outputRecoveries.fetch_add(1, std::memory_order_relaxed); break;
        case RecoveryCause::Stall: stallEvents.fetch_add(1, std::memory_order_relaxed); break;
        case RecoveryCause::Sequence: sequenceRecoveries.fetch_add(1, std::memory_order_relaxed); break;
        case RecoveryCause::Timestamp: timestampRecoveries.fetch_add(1, std::memory_order_relaxed); break;
        case RecoveryCause::SourceSession: sourceSessionRecoveries.fetch_add(1, std::memory_order_relaxed); break;
        case RecoveryCause::SourceRate: sourceRateRecoveries.fetch_add(1, std::memory_order_relaxed); break;
        }
        reconnects.fetch_add(1, std::memory_order_relaxed);
    }
};
}
