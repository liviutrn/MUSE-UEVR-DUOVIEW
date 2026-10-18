// DuoViewConfig.hpp — validated configuration snapshot for the DuoView hijack.
//
// The engine-facing settings (ImGui sliders/toggles in VR.cpp) write into a
// plain struct; this header validates/clamps it into an immutable snapshot
// that the render path uses. The snapshot is produced once per frame and
// shared by the projection hook and the D3D12 composite, so they can never
// disagree mid-frame.
//
// Pure logic (no UE, no D3D): unit-tested on host.
#pragma once

#include <cstdint>

namespace uevr::duoview {

enum class FoveaMode : uint8_t {
    FIXED_CENTER = 0, // fovea locked at image center (default, v1 focus)
    GAZE_TRACKED = 1, // fovea follows the stabilized gaze point
};

struct RawConfig {
    FoveaMode fovea_mode = FoveaMode::FIXED_CENTER;
    float crop_fraction = 0.30f;   // foveal ROI size as fraction of full FOV
    float periphery_scale = 0.25f;  // linear downscale of the periphery view
    float feather_px = 12.0f;       // fovea/periphery blend band, output pixels
    bool debug_border = false;      // draw a visible ROI border (diagnostic)
};

// Ranges are part of the contract: the UI clamps to these, and so does
// validate(), so a corrupt config file can never produce a degenerate frame.
struct ValidatedConfig {
    FoveaMode fovea_mode = FoveaMode::FIXED_CENTER;
    float crop_fraction = 0.30f;
    float periphery_scale = 0.25f;
    float feather_px = 12.0f;
    bool debug_border = false;

    static constexpr float kCropMin = 0.20f, kCropMax = 0.50f;
    static constexpr float kPeripheryMin = 0.15f, kPeripheryMax = 0.50f;
    static constexpr float kFeatherMin = 0.0f, kFeatherMax = 64.0f;

    static ValidatedConfig validate(const RawConfig& in) {
        ValidatedConfig out{};
        out.fovea_mode = in.fovea_mode;
        out.crop_fraction = clamp(in.crop_fraction, kCropMin, kCropMax);
        out.periphery_scale = clamp(in.periphery_scale, kPeripheryMin, kPeripheryMax);
        out.feather_px = clamp(in.feather_px, kFeatherMin, kFeatherMax);
        out.debug_border = in.debug_border;
        return out;
    }

    // Periphery viewport size for one eye slot of W x H.
    void periphery_extent(uint32_t slot_w, uint32_t slot_h,
                          uint32_t& out_w, uint32_t& out_h) const {
        out_w = static_cast<uint32_t>(slot_w * periphery_scale);
        out_h = static_cast<uint32_t>(slot_h * periphery_scale);
        if (out_w < 16) out_w = 16;
        if (out_h < 16) out_h = 16;
        if (out_w > slot_w) out_w = slot_w;
        if (out_h > slot_h) out_h = slot_h;
    }

private:
    static float clamp(float x, float lo, float hi) {
        return x < lo ? lo : (x > hi ? hi : x);
    }
};

// Immutable per-frame state shared by the projection hook and the composite.
// Written once per rendered frame (under a mutex), read by the submit path.
struct FrameSnapshot {
    uint64_t frame_id = 0;   // engine frame this snapshot belongs to
    bool valid = false;
    ValidatedConfig config{};
    // Stabilized fovea center in common-basis UV (fixed => 0.5, 0.5).
    float fovea_u = 0.5f;
    float fovea_v = 0.5f;
};

} // namespace uevr::duoview
