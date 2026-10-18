// Host unit tests for the DuoView pure-logic headers.
// Build: g++ -std=c++20 -Wall -Wextra -Werror -I.. duoview_tests.cpp -o duoview_tests
#include <cstdio>
#include <cmath>

#include "DuoViewGeometry.hpp"
#include "DuoViewGaze.hpp"
#include "DuoViewConfig.hpp"

static int g_fail = 0;
static int g_pass = 0;
#define CHECK(cond) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)
#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))

using namespace uevr::duoview;

// ---------------------------------------------------------------- geometry
void test_symmetric_crop_reproduces_common() {
    // s = 1.0 centered crop must be exactly the common symmetric matrix.
    const float h = 1.2f, v = 0.9f, near = 0.1f;
    const auto crop = make_crop_rect(0.5f, 0.5f, 1.0f);
    const auto ct = crop_tangents(h, v, crop);
    const auto m = crop_projection_matrix<float>(ct, near);
    const auto ref = common_projection_matrix<float>(h, v, near);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            CHECK_NEAR(m[c][r], ref[c][r], 1e-6f);
}

void test_crop_corners_map_to_ndc_edges() {
    const float h = 1.1f, v = 0.8f, near = 0.1f;
    const auto crop = make_crop_rect(0.5f, 0.5f, 0.3f);
    const auto ct = crop_tangents(h, v, crop);
    const auto m = crop_projection_matrix<float>(ct, near);
    const float z = 10.0f;
    // ROI corners (tangent space) must land on NDC +-1.
    struct Corner { float tx, ty, ex, ey; };
    const Corner corners[] = {
        {ct.l, ct.b, -1.f, -1.f}, {ct.r, ct.b, 1.f, -1.f},
        {ct.l, ct.t, -1.f, 1.f},  {ct.r, ct.t, 1.f, 1.f},
        {(ct.l + ct.r) * 0.5f, (ct.b + ct.t) * 0.5f, 0.f, 0.f},
    };
    for (const auto& c : corners) {
        const auto ndc = transform_point(m, c.tx * z, c.ty * z, z);
        CHECK_NEAR(ndc[0], c.ex, 1e-5f);
        CHECK_NEAR(ndc[1], c.ey, 1e-5f);
        CHECK_NEAR(ndc[2], near / z, 1e-6f); // infinite-far depth convention
    }
    // A point halfway between ROI edge and full-frustum edge is outside [-1,1].
    const float ox = ct.r + (h - ct.r) * 0.5f;
    const auto ndc = transform_point(m, ox * z, 0.f, z);
    CHECK(ndc[0] > 1.0f);
}

void test_offcenter_crop() {
    const float h = 1.0f, v = 1.0f, near = 0.1f;
    // Gaze at 75% width, 25% height.
    const auto crop = make_crop_rect(0.75f, 0.25f, 0.3f);
    CHECK_NEAR(crop.u0, 0.60f, 1e-6f);
    CHECK_NEAR(crop.u1, 0.90f, 1e-6f);
    const auto ct = crop_tangents(h, v, crop);
    const auto m = crop_projection_matrix<float>(ct, near);
    const float z = 5.0f;
    // ROI center must project to NDC origin.
    const auto ndc = transform_point(m, (ct.l + ct.r) * 0.5f * z, (ct.b + ct.t) * 0.5f * z, z);
    CHECK_NEAR(ndc[0], 0.f, 1e-5f);
    CHECK_NEAR(ndc[1], 0.f, 1e-5f);
    // Asymmetric: left/right extents differ.
    CHECK(std::fabs(ct.r) != std::fabs(ct.l));
}

void test_crop_clamp_at_edges() {
    // Center near the edge: rect must stay inside [0,1].
    const auto crop = make_crop_rect(0.05f, 0.95f, 0.3f);
    CHECK(crop.u0 >= 0.f && crop.u1 <= 1.f);
    CHECK(crop.v0 >= 0.f && crop.v1 <= 1.f);
    CHECK(crop.width() > 0.f && crop.height() > 0.f);
}

void test_uv_roundtrip() {
    const auto crop = make_crop_rect(0.6f, 0.4f, 0.3f);
    float fu, fv, u, v;
    CHECK(common_uv_to_fovea(0.65f, 0.42f, crop, fu, fv));
    fovea_uv_to_common(fu, fv, crop, u, v);
    CHECK_NEAR(u, 0.65f, 1e-6f);
    CHECK_NEAR(v, 0.42f, 1e-6f);
    // Outside the crop -> false.
    CHECK(!common_uv_to_fovea(0.1f, 0.1f, crop, fu, fv));
}

void test_double_precision_matches_float() {
    const float h = 1.15f, v = 0.85f;
    const auto crop = make_crop_rect(0.4f, 0.6f, 0.35f);
    const auto ctf = crop_tangents(h, v, crop);
    const auto mf = crop_projection_matrix<float>(ctf, 0.1f);
    const auto md = crop_projection_matrix<double>(ctf, 0.1);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            CHECK_NEAR(mf[c][r], (float)md[c][r], 1e-6f);
}

// Regression: slow drift must accumulate via the smooth EMA state.
// Without it, sub-cell steps never accumulated and the fovea stalled.
void test_gaze_slow_drift_accumulates() {
    GazeStabilizer st;
    uint64_t t = 1000000;
    float u = 0.5f, v = 0.5f;
    st.update({0.5f, 0.5f, true}, t, u, v);
    CHECK(u == 0.5f);
    // Drift 0.001 UV/frame for 200 frames (0.2 UV total). Each step is below
    // the deadband, but the cumulative drift far exceeds it.
    for (int i = 1; i <= 200; ++i) {
        t += 16667;
        st.update({0.5f + i * 0.001f, 0.5f, true}, t, u, v);
    }
    CHECK(u > 0.5f);
}

// ---------------------------------------------------------------- gaze
void test_gaze_first_sample_snaps() {
    GazeStabilizer st;
    float u, v;
    st.update({0.62f, 0.41f, true}, 1000, u, v);
    // Snapped to 1/64 grid.
    CHECK_NEAR(u, std::round(0.62f * 64) / 64, 1e-6f);
    CHECK_NEAR(v, std::round(0.41f * 64) / 64, 1e-6f);
    CHECK(st.has_output());
}

void test_gaze_deadband_holds() {
    GazeStabilizer st;
    float u, v, u0, v0;
    st.update({0.5f, 0.5f, true}, 1000, u0, v0);
    // Jitter well inside the deadband: output must not move at all.
    for (int i = 0; i < 20; ++i) {
        const float jx = 0.5f + 0.003f * ((i % 2) ? 1.f : -1.f);
        const float jy = 0.5f + 0.002f * ((i % 3) ? 1.f : -1.f);
        st.update({jx, jy, true}, 2000 + i * 1000, u, v);
        CHECK(u == u0 && v == v0);
    }
}

void test_gaze_saccade_moves_fast() {
    GazeStabilizer st;
    float u, v;
    st.update({0.5f, 0.5f, true}, 1000, u, v);
    // Big jump: output must leave the start cell within a few updates.
    bool moved = false;
    for (int i = 0; i < 10; ++i) {
        st.update({0.8f, 0.5f, true}, 2000 + i * 1000, u, v);
        if (u != 0.5f) { moved = true; break; }
    }
    CHECK(moved);
    // ...and converge near the target (quantized).
    for (int i = 0; i < 200; ++i) st.update({0.8f, 0.5f, true}, 20000 + i * 1000, u, v);
    CHECK(std::fabs(u - std::round(0.8f * 64) / 64) < 1e-6f);
}

void test_gaze_output_always_on_grid() {
    GazeStabilizer st;
    float u, v;
    // Random walk; every published output must be on the 1/64 grid.
    float x = 0.5f;
    for (int i = 0; i < 500; ++i) {
        x += 0.004f * ((i * 37 % 11) - 5);
        if (x < 0.05f) { x = 0.05f; } if (x > 0.95f) { x = 0.95f; }
        st.update({x, 0.5f, true}, 1000 + i * 1000, u, v);
        CHECK(std::fabs(u * 64 - std::round(u * 64)) < 1e-5f);
        CHECK(std::fabs(v * 64 - std::round(v * 64)) < 1e-5f);
    }
}

void test_gaze_invalid_holds_and_stales() {
    GazeStabilizer st;
    float u, v, u0, v0;
    st.update({0.6f, 0.6f, true}, 1000, u0, v0);
    CHECK(!st.stale(2000));
    // Invalid samples hold the last output.
    for (int i = 0; i < 5; ++i) {
        st.update({0.1f, 0.1f, false}, 2000 + i * 1000, u, v);
        CHECK(u == u0 && v == v0);
    }
    // Past the 250 ms hold -> stale (caller falls back to center).
    // A blink (100-150 ms) must NOT trigger stale.
    CHECK(!st.stale(1000 + 150000));
    CHECK(st.stale(1000 + 260000));
}

// ---------------------------------------------------------------- config
void test_config_validation() {
    RawConfig raw{};
    raw.crop_fraction = 5.0f;      // absurd
    raw.periphery_scale = -1.0f;   // absurd
    raw.feather_px = 1000.f;
    const auto c = ValidatedConfig::validate(raw);
    CHECK(c.crop_fraction == ValidatedConfig::kCropMax);
    CHECK(c.periphery_scale == ValidatedConfig::kPeripheryMin);
    CHECK(c.feather_px == ValidatedConfig::kFeatherMax);
    CHECK(c.fovea_mode == FoveaMode::FIXED_CENTER);
}

void test_periphery_extent() {
    ValidatedConfig c{};
    uint32_t w, h;
    c.periphery_extent(2000, 2000, w, h);
    CHECK(w == 500 && h == 500);
    c.periphery_scale = 0.15f;
    c.periphery_extent(2000, 1000, w, h);
    CHECK(w == 300 && h == 150);
}

int main() {
    test_symmetric_crop_reproduces_common();
    test_crop_corners_map_to_ndc_edges();
    test_offcenter_crop();
    test_crop_clamp_at_edges();
    test_uv_roundtrip();
    test_double_precision_matches_float();
    test_gaze_slow_drift_accumulates();
    test_gaze_first_sample_snaps();
    test_gaze_deadband_holds();
    test_gaze_saccade_moves_fast();
    test_gaze_output_always_on_grid();
    test_gaze_invalid_holds_and_stales();
    test_config_validation();
    test_periphery_extent();
    std::printf("pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
