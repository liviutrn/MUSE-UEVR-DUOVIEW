// DuoViewGaze.hpp — gaze stabilization for the DuoView fovea.
//
// Background (user-verified): the previous eye-tracked implementation fed raw
// gaze into the foveal projection every frame. The result trembled badly — the
// foveal *image* shook, not just the gaze point — and it continuously
// invalidated TAA/Lumen temporal history.
//
// This stabilizer sits between the raw eye tracker and the projection:
//   raw sample -> adaptive smoothing -> deadband -> grid quantization -> latch
//
// * Adaptive smoothing: fast during real saccades, slow during fixation.
// * Deadband: displacements below the threshold do not move the output at all.
// * Grid quantization: the output lives on a coarse grid, so the projection
//   matrix only changes on grid crossings and temporal history stays valid.
// * Latch: the render path snapshots the output once per frame; the projection
//   hook and the composite always agree.
//
// Pure logic (no UE, no D3D): unit-tested on host.
#pragma once

#include <cmath>
#include <cstdint>

namespace uevr::duoview {

struct GazeSample {
    float u{};      // gaze point in common-basis image UV [0,1]
    float v{};
    bool valid{};   // tracker produced a usable sample this frame
};

class GazeStabilizer {
public:
    struct Params {
        // Adaptive EMA: alpha lerps from slow_alpha (fixation) to fast_alpha
        // (saccade) based on the input displacement.
        float slow_alpha = 0.08f;   // gentle drift during fixation
        float fast_alpha = 0.65f;   // quick catch-up during saccades
        // Displacements are measured in UV units; saccade_blend is the
        // displacement at which we fully switch to fast_alpha.
        float saccade_blend = 0.08f;
        // Deadband: ignore input motion smaller than this (UV units).
        float deadband = 0.008f;
        // Quantization grid: output is snapped to 1/grid_divisions.
        // 64 -> the projection changes at most on 1/64-FOV crossings.
        int grid_divisions = 64;
        // Freshness: if no valid sample arrives for longer than this,
        // the stabilizer reports stale (caller falls back to center).
        uint64_t stale_timeout_us = 50000; // 50 ms, matches v22 CAPI hold
    };

    GazeStabilizer() { reset(); }
    explicit GazeStabilizer(const Params& p) : params_(p) { reset(); }

    void reset() {
        has_output_ = false;
        out_u_ = 0.5f; out_v_ = 0.5f;
        smooth_u_ = 0.5f; smooth_v_ = 0.5f;
        last_valid_us_ = 0;
        ever_valid_ = false;
    }

    // Feed one tracker sample. Returns the stabilized output UV.
    // `now_us` is a monotonic clock (microseconds).
    void update(const GazeSample& s, uint64_t now_us, float& out_u, float& out_v) {
        if (s.valid && std::isfinite(s.u) && std::isfinite(s.v)) {
            last_valid_us_ = now_us;
            ever_valid_ = true;
            const float cu = clamp01(s.u), cv = clamp01(s.v);
            if (!has_output_) {
                // First valid sample: snap immediately (no one is watching yet).
                smooth_u_ = cu; smooth_v_ = cv;
                out_u_ = quantize(cu); out_v_ = quantize(cv);
                has_output_ = true;
            } else {
                // Measure against the smooth (non-quantized) position so the
                // EMA accumulates across frames even when the quantized output
                // hasn't moved yet.
                const float dx = cu - smooth_u_, dy = cv - smooth_v_;
                const float dist = std::sqrt(dx*dx + dy*dy);
                if (dist >= params_.deadband) {
                    // Adaptive blend: 0 at deadband edge -> 1 at saccade_blend.
                    float t = (dist - params_.deadband) /
                              std::fmax(params_.saccade_blend - params_.deadband, 1e-6f);
                    t = clamp01(t);
                    const float alpha = params_.slow_alpha +
                        t * (params_.fast_alpha - params_.slow_alpha);
                    smooth_u_ = clamp01(smooth_u_ + alpha * dx);
                    smooth_v_ = clamp01(smooth_v_ + alpha * dy);
                    // Quantize for output. Only publish when the quantized
                    // value actually changed: this is what keeps the projection
                    // matrix bit-stable during fixation.
                    const float nu = quantize(smooth_u_);
                    const float nv = quantize(smooth_v_);
                    if (nu != out_u_ || nv != out_v_) {
                        out_u_ = nu; out_v_ = nv;
                    }
                }
            }
        }
        out_u = out_u_;
        out_v = out_v_;
    }

    // True when we have never seen a valid sample, or the last one is too old.
    bool stale(uint64_t now_us) const {
        if (!ever_valid_) return true;
        return now_us - last_valid_us_ > params_.stale_timeout_us;
    }

    bool has_output() const { return has_output_; }

private:
    static float clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }

    float quantize(float x) const {
        const float g = (float)params_.grid_divisions;
        return std::round(clamp01(x) * g) / g;
    }

    Params params_;
    bool has_output_{};
    bool ever_valid_{};
    float out_u_{}, out_v_{};
    // Non-quantized EMA state. The EMA accumulates here even when the
    // quantized output is stable; without this, sub-cell steps would
    // never accumulate and slow drifts would stall forever.
    float smooth_u_{}, smooth_v_{};
    uint64_t last_valid_us_{};
};

} // namespace uevr::duoview
