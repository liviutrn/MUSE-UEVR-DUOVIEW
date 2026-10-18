// DuoViewManager.hpp — per-frame DuoView state (Windows-only).
//
// Owns the immutable per-frame snapshot shared by the projection hook
// (game thread, slot 0) and the D3D12 composite (render thread):
//   - validated config (from the UI settings),
//   - fovea center (fixed 0.5,0.5, or the stabilized gaze point),
//   - crop rect in common-basis UV, derived from the common projection.
//
// The snapshot is written once per frame by update() and read via snapshot().
// The foveal projection for slot 0 is built from the same snapshot, so the
// projection hook and the composite can never disagree mid-frame.
//
// Forward-declares VR to avoid a circular include; the .cpp includes VR.hpp.
#pragma once

#ifdef _WIN32

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

#include <sdk/Math.hpp>

#include "DuoViewConfig.hpp"
#include "DuoViewGeometry.hpp"
#include "DuoViewGaze.hpp"
#include "DuoViewPsvr2Gaze.hpp"

class VR;

namespace uevr::duoview {

class Manager {
public:
    Manager() = default;
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;

    // Refresh the snapshot. Called from the projection hook for slot 0
    // (game thread). `common` is the runtime's common mono projection
    // (column-major, +Z-forward); its [0][0]/[1][1] carry 1/h and 1/v and
    // its [3][2] carries the near plane.
    void update(VR* vr, const Matrix4x4f& common);

    // Latest snapshot (thread-safe copy). For the D3D12 composite and submit.
    FrameSnapshot snapshot() const;

    // Foveal crop projection for slot 0, from the snapshot's crop rect.
    // Nullopt when no valid snapshot exists yet (composite falls back to
    // duplicating the periphery — degraded mono, never a black frame).
    std::optional<Matrix4x4f> foveal_projection() const;

private:
    static uint64_t now_us() {
        return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    mutable std::mutex mutex_;
    FrameSnapshot snapshot_{};
    CropRect crop_{};
    float common_h_{};
    float common_v_{};
    float nearz_{0.1f};
    uint64_t frame_seq_{};
    GazeStabilizer stabilizer_{};
    psvr2::Reader gaze_reader_{};
};

} // namespace uevr::duoview

#endif // _WIN32
