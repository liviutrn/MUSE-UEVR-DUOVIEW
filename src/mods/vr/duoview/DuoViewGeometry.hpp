// DuoViewGeometry.hpp — foveal crop projection math for the DuoView stereo-slot hijack.
//
// Conventions (must match the UEVR baseline exactly):
//   - Matrices are COLUMN-MAJOR, glm-style: m[col][row], 16-float initializer lists
//     run column by column. (Verified against the baseline's common mono matrix:
//     Matrix4x4f{1/h,0,0,0, 0,1/v,0,0, 0,0,0,near, 0,0,1,0}.)
//   - +Z-forward view space, infinite far plane: NDC = (x/(h*z), y/(v*z), near/z).
//     In particular m[2][3] == 1 (the v22 "+Z-forward" bugfix — do not flip this).
//   - The "common basis" is Joey's common mono projection: symmetric tangents
//     (h, v) covering both eye frusta, image UV [0,1] across the full basis.
//
// This header is pure logic (no UE, no D3D): it is unit-tested on host.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace uevr::duoview {

// ---------------------------------------------------------------------------
// Crop description in the common-basis image UV space [0,1].
// ---------------------------------------------------------------------------
struct CropRect {
    float u0{}, v0{}; // top-left in common UV
    float u1{}, v1{}; // bottom-right in common UV

    constexpr float width() const { return u1 - u0; }
    constexpr float height() const { return v1 - v0; }
    constexpr bool contains(float u, float v) const {
        return u >= u0 && u <= u1 && v >= v0 && v <= v1;
    }
};

// Build the crop rect from a center (common UV) and a size fraction s.
// The rect is clamped so it never leaves [0,1]; if clamping shifts an edge,
// the rect shrinks (center is preserved as much as possible, then clamped).
inline CropRect make_crop_rect(float center_u, float center_v, float s) {
    CropRect r{};
    const float half = 0.5f * s;
    r.u0 = center_u - half; r.u1 = center_u + half;
    r.v0 = center_v - half; r.v1 = center_v + half;
    // Clamp into [0,1] while keeping the rect as large as possible.
    if (r.u0 < 0.f) { r.u1 -= r.u0; r.u0 = 0.f; }
    if (r.u1 > 1.f) { r.u0 -= (r.u1 - 1.f); r.u1 = 1.f; }
    if (r.v0 < 0.f) { r.v1 -= r.v0; r.v0 = 0.f; }
    if (r.v1 > 1.f) { r.v0 -= (r.v1 - 1.f); r.v1 = 1.f; }
    if (r.u0 < 0.f) r.u0 = 0.f;
    if (r.v0 < 0.f) r.v0 = 0.f;
    return r;
}

// ---------------------------------------------------------------------------
// Frustum tangents of the crop, in the common basis.
// The common basis has symmetric half-tangents (h, v): full frustum
// [-h, +h] x [-v, +v] maps to common UV [0,1] x [0,1].
// ---------------------------------------------------------------------------
struct CropTangents {
    float l{}, r{}, b{}, t{}; // left, right, bottom, top tangent bounds
};

inline CropTangents crop_tangents(float h, float v, const CropRect& crop) {
    CropTangents ct{};
    ct.l = (2.f * crop.u0 - 1.f) * h;
    ct.r = (2.f * crop.u1 - 1.f) * h;
    ct.b = (1.f - 2.f * crop.v1) * v; // v=1 is top in image UV
    ct.t = (1.f - 2.f * crop.v0) * v;
    return ct;
}

// ---------------------------------------------------------------------------
// Off-axis perspective matrix for the crop, same depth convention as the
// baseline's common matrix: NDC_z = near / z_view (infinite far).
// Stored column-major: m[col][row].
// ---------------------------------------------------------------------------
template <typename T>
inline std::array<std::array<T, 4>, 4> crop_projection_matrix(
    const CropTangents& ct, T near_plane)
{
    const T l = (T)ct.l, r = (T)ct.r, b = (T)ct.b, t = (T)ct.t;
    const T rl = r - l, tb = t - b;
    std::array<std::array<T, 4>, 4> m{};
    // Column 0
    m[0][0] = (T)2 / rl; m[0][1] = 0; m[0][2] = 0; m[0][3] = 0;
    // Column 1
    m[1][0] = 0; m[1][1] = (T)2 / tb; m[1][2] = 0; m[1][3] = 0;
    // Column 2
    m[2][0] = -(r + l) / rl; m[2][1] = -(t + b) / tb; m[2][2] = 0; m[2][3] = (T)1;
    // Column 3
    m[3][0] = 0; m[3][1] = 0; m[3][2] = near_plane; m[3][3] = 0;
    return m;
}

// The full-basis symmetric matrix (what the periphery slot uses).
// Must be bit-consistent with the baseline's common mono matrix layout.
template <typename T>
inline std::array<std::array<T, 4>, 4> common_projection_matrix(
    T h, T v, T near_plane)
{
    std::array<std::array<T, 4>, 4> m{};
    m[0][0] = (T)1 / h;
    m[1][1] = (T)1 / v;
    m[2][3] = (T)1;
    m[3][2] = near_plane;
    return m;
}

// Transform a view-space point by a column-major matrix, return NDC.
template <typename T>
inline std::array<T, 3> transform_point(
    const std::array<std::array<T, 4>, 4>& m, T x, T y, T z)
{
    const T xc = m[0][0]*x + m[1][0]*y + m[2][0]*z + m[3][0];
    const T yc = m[0][1]*x + m[1][1]*y + m[2][1]*z + m[3][1];
    const T zc = m[0][2]*x + m[1][2]*y + m[2][2]*z + m[3][2];
    const T wc = m[0][3]*x + m[1][3]*y + m[2][3]*z + m[3][3];
    return {xc / wc, yc / wc, zc / wc};
}

// Map a fovea-slot UV [0,1] (the crop rendered across the full slot) back to
// common-basis UV. Used by the composite shader (mirrored in HLSL).
inline void fovea_uv_to_common(float fu, float fv, const CropRect& crop,
                               float& out_u, float& out_v)
{
    out_u = crop.u0 + fu * crop.width();
    out_v = crop.v0 + fv * crop.height();
}

// Map a common-basis UV to fovea-slot UV. Returns false when outside the crop.
inline bool common_uv_to_fovea(float u, float v, const CropRect& crop,
                               float& out_fu, float& out_fv)
{
    if (!crop.contains(u, v) || crop.width() <= 0.f || crop.height() <= 0.f) {
        return false;
    }
    out_fu = (u - crop.u0) / crop.width();
    out_fv = (v - crop.v0) / crop.height();
    return true;
}

} // namespace uevr::duoview
