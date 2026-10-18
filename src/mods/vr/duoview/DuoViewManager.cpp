// DuoViewManager.cpp — per-frame DuoView state implementation.
#include "DuoViewManager.hpp"

#include "../../VR.hpp"

namespace uevr::duoview {

void Manager::update(VR* vr, const Matrix4x4f& common) {
    if (vr == nullptr) {
        return;
    }
    const auto config = vr->duoview_config();

    // Extract the common optical basis. Guard against a degenerate matrix
    // (never publish a crop derived from garbage).
    const float m00 = common[0][0], m11 = common[1][1], m32 = common[3][2];
    if (!std::isfinite(m00) || !std::isfinite(m11) || !std::isfinite(m32) ||
        m00 == 0.0f || m11 == 0.0f || m32 <= 0.0f) {
        return;
    }
    const float h = 1.0f / m00;
    const float v = 1.0f / m11;
    if (!(h > 0.0f) || !(v > 0.0f) || !std::isfinite(h) || !std::isfinite(v)) {
        return;
    }

    // Fovea center in common-basis UV.
    float fu = 0.5f, fv = 0.5f;
    if (config.fovea_mode == FoveaMode::GAZE_TRACKED) {
        const uint64_t now = now_us();
        GazeSample sample{};
        if (const auto ray = gaze_reader_.read()) {
            // The CAPI ray is HMD-local. The mono camera sits at the HMD pose
            // up to UEVR's rotation offset, so rotating by the inverse offset
            // lands the ray in the camera's view space (the HMD rotation
            // cancels out of the relative transform).
            const auto inv = glm::inverse(vr->get_rotation_offset());
            const auto gv = inv * glm::vec3{ray->x, ray->y, ray->z};
            if (gv.z > 0.01f && std::isfinite(gv.x) && std::isfinite(gv.y) && std::isfinite(gv.z)) {
                sample.u = (gv.x / (h * gv.z) + 1.0f) * 0.5f;
                sample.v = (1.0f - gv.y / (v * gv.z)) * 0.5f;
                sample.valid = true;
            }
        }
        stabilizer_.update(sample, now, fu, fv);
        if (stabilizer_.stale(now)) {
            // Tracker genuinely lost (not a blink): fall back to center.
            // Reset so the next valid sample takes the clean first-snap path
            // instead of jumping from a stale position.
            if (!was_stale_) {
                stabilizer_.reset();
            }
            fu = 0.5f; fv = 0.5f;
            was_stale_ = true;
        } else {
            was_stale_ = false;
        }
    } else {
        stabilizer_.reset();
    }

    const auto crop = make_crop_rect(fu, fv, config.crop_fraction);

    std::scoped_lock lock{mutex_};
    snapshot_.frame_id = ++frame_seq_;
    snapshot_.valid = true;
    snapshot_.config = config;
    snapshot_.fovea_u = fu;
    snapshot_.fovea_v = fv;
    crop_ = crop;
    common_h_ = h;
    common_v_ = v;
    nearz_ = m32;
}

FrameSnapshot Manager::snapshot() const {
    std::scoped_lock lock{mutex_};
    return snapshot_;
}

std::optional<Matrix4x4f> Manager::foveal_projection() const {
    std::scoped_lock lock{mutex_};
    if (!snapshot_.valid || common_h_ <= 0.0f || common_v_ <= 0.0f) {
        return std::nullopt;
    }
    const auto ct = crop_tangents(common_h_, common_v_, crop_);
    const auto m = crop_projection_matrix<float>(ct, nearz_);
    Matrix4x4f out{};
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            out[c][r] = m[c][r];
        }
    }
    return out;
}

} // namespace uevr::duoview
