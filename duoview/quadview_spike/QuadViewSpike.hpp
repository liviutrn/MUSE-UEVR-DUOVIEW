#pragma once
// QuadView prototype spike: 4-view (L/R x context/focus) production.
//
// This is a SEPARATE experimental path alongside the ID6 DuoView renderer.
// It bypasses Joey's native stereo fix for the prototype (the fix's 2-view
// capture machinery is not generalized here). When the `quadview_spike_enabled`
// setting is false, all spike code is inert and the build behaves exactly as
// the 176 baseline.
//
// View layout (2x2 grid in the engine's view array):
//   0 = LEFT  context (full FOV, low density)
//   1 = RIGHT context (full FOV, low density)
//   2 = LEFT  focus   (foveal crop, high density)
//   3 = RIGHT focus   (foveal crop, high density)
//
// The prototype submits views 0+1 as plain stereo (proving 4 views render);
// views 2+3 are rendered but not submitted. Diagnostics confirm all 4.
// Phase 1 will add the per-eye D3D12 composite (4 -> 2 stereo).

#include <atomic>
#include <cstdint>

namespace uevr::quadview_spike {

inline constexpr uint32_t kViewCount = 4;
inline constexpr uint32_t kLeftContext = 0;
inline constexpr uint32_t kRightContext = 1;
inline constexpr uint32_t kLeftFocus = 2;
inline constexpr uint32_t kRightFocus = 3;

inline constexpr bool is_focus_view(uint32_t view) {
    return view == kLeftFocus || view == kRightFocus;
}
inline constexpr bool is_right_eye(uint32_t view) {
    return view == kRightContext || view == kRightFocus;
}
inline constexpr uint32_t eye_for_view(uint32_t view) {
    return is_right_eye(view) ? 1u : 0u; // 0=left, 1=right
}

// ---------------------------------------------------------------------------
// Diagnostics: per-view production counters, read from the UI / log.
// ---------------------------------------------------------------------------
struct Counters {
    // Frames where the engine was asked for 4 views.
    std::atomic<uint64_t> frames_requested{0};
    // adjust_view_rect calls per view index.
    std::atomic<uint64_t> view_rect_calls[kViewCount]{};
    // calculate_stereo_projection_matrix calls per view index.
    std::atomic<uint64_t> projection_calls[kViewCount]{};
    // Frames where all 4 views got both rect and projection.
    std::atomic<uint64_t> frames_complete{0};
    // Last observed view count from the engine (for mismatch detection).
    std::atomic<uint32_t> last_observed_view_count{0};
};

inline Counters& counters() {
    static Counters c;
    return c;
}

// Called once per frame after the view family is observed. Returns true when
// all 4 views were produced (rect + projection) this frame.
inline bool note_frame_complete(bool all_four) {
    if (all_four) counters().frames_complete.fetch_add(1, std::memory_order_relaxed);
    return all_four;
}

} // namespace uevr::quadview_spike
