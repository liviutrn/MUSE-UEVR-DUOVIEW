// DuoViewDiagnostics.hpp — counters and throttled logging for DuoView.
// Lets us see what happened when something doesn't work, without spamming
// the log every frame. Counters are shown in the UEVR UI (DuoView section).
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace uevr::duoview {

struct Diagnostics {
    std::atomic<uint64_t> frames_total{0};
    std::atomic<uint64_t> composites_ok{0};        // full foveated composite
    std::atomic<uint64_t> fallbacks_used{0};       // periphery-only fallback
    std::atomic<uint64_t> composite_failed{0};     // both attempts failed
    std::atomic<uint64_t> init_attempts{0};
    std::atomic<uint64_t> init_success{0};
    std::atomic<uint64_t> snapshots_synthesized{0}; // first-frame defaults
    std::atomic<uint64_t> gaze_stale_entries{0};    // tracker-lost transitions
    std::atomic<uint64_t> gaze_recoveries{0};       // stale -> valid

    // Last error (short, for the UI). Written under a lock-free best-effort
    // scheme: only the most recent matters.
    static constexpr size_t kMaxErr = 256;
    std::atomic<uint64_t> error_seq{0};
    char last_error[kMaxErr]{};

    void set_error(const char* msg) {
        if (msg == nullptr) return;
        size_t i = 0;
        for (; i + 1 < kMaxErr && msg[i] != '\0'; ++i) {
            last_error[i] = msg[i];
        }
        last_error[i] = '\0';
        error_seq.fetch_add(1, std::memory_order_relaxed);
    }

    void reset() {
        frames_total = 0;
        composites_ok = 0;
        fallbacks_used = 0;
        composite_failed = 0;
        init_attempts = 0;
        init_success = 0;
        snapshots_synthesized = 0;
        gaze_stale_entries = 0;
        gaze_recoveries = 0;
        last_error[0] = '\0';
    }
};

// Global instance (owned by VR, accessed from render/game threads via atomics).
inline Diagnostics& diagnostics() {
    static Diagnostics d;
    return d;
}

} // namespace uevr::duoview
