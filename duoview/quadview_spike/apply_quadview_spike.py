#!/usr/bin/env python3
"""QuadView prototype spike: 4-view (L/R x context/focus) production.

Applies ON TOP of a 176-patched Joey tree (stages 1+234). Adds a separate
experimental path alongside ID6 DuoView. When `quadview_spike_enabled` is
false (default), this patch is inert.

Usage: python3 apply_quadview_spike.py <joey_root> <duoview_src>
  <joey_root>: Joey UEVR checkout with stages 1+234 already applied.
  <duoview_src>: duoview/ directory containing quadview_spike/QuadViewSpike.hpp.
"""
import sys
from pathlib import Path

def replace(text, old, new, name):
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{name}: expected one occurrence, found {count}: {old[:95]!r}")
    return text.replace(old, new)

def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    root = Path(sys.argv[1]).resolve()
    src = Path(sys.argv[2]).resolve()
    changed = {}

    def source_root(path):
        # backend sources live under root; frontend not touched by spike.
        return root

    def edit(path, before, after):
        s = changed.get(path)
        if s is None:
            s = (source_root(path) / path).read_text(encoding="utf-8-sig")
        changed[path] = replace(s, before, after, path)

    # --- 1. Copy the spike header into the tree ---
    target = root / "src/mods/vr/QuadViewSpike.hpp"
    if target.exists():
        raise RuntimeError("Refusing to overwrite prior source: " + str(target))
    target.write_text((src / "quadview_spike/QuadViewSpike.hpp").read_text(encoding="utf-8"),
                      encoding="utf-8", newline="")

    # --- 2. Add the setting to DuoViewConfig::Settings ---
    edit("src/mods/vr/DuoViewConfig.hpp",
         "    bool native_fix_same_pass{true};",
         "    bool native_fix_same_pass{true};\n"
         "    // QuadView prototype spike: when true, produce 4 views (L/R x context/focus)\n"
         "    // as a separate experimental path. Default false = inert.\n"
         "    bool quadview_spike_enabled{false};")

    # --- 2b. Add the UI toggle (VR.hpp member) ---
    edit("src/mods/VR.hpp",
         '    const ModToggle::Ptr m_duo_native_fix{ModToggle::create(generate_name("DuoViewNativeFixSamePass"), true)};',
         '    const ModToggle::Ptr m_duo_native_fix{ModToggle::create(generate_name("DuoViewNativeFixSamePass"), true)};\n'
         '    const ModToggle::Ptr m_quadview_spike{ModToggle::create(generate_name("QuadViewSpikeEnabled"), false)};')

    # --- 2c. Draw the UI checkbox (after the Native Fix shading toggle) ---
    edit("src/mods/VR.cpp",
         '                m_duo_native_fix->draw("Native Stereo Fix shading (OFF = stock secondary-pass)");',
         '                m_duo_native_fix->draw("Native Stereo Fix shading (OFF = stock secondary-pass)");\n'
         '                m_quadview_spike->draw("QuadView spike: 4-view prototype (L/R x context/focus)");\n'
         '                ImGui::TextWrapped("Experimental. Renders 4 views in a 2x2 grid (top: L/R context, bottom: L/R focus crop). Submits via the normal path; diagnostics count the 4 views. Disable ID6 DuoView first.");')

    # --- 2d. Wire the toggle to Settings ---
    edit("src/mods/VR.cpp",
         "        .native_fix_same_pass=m_duo_native_fix->value(),",
         "        .native_fix_same_pass=m_duo_native_fix->value(),\n"
         "        .quadview_spike_enabled=m_quadview_spike->value(),")

    # --- 2e. Reconcile the new setting in DuoViewConfig ---
    edit("src/mods/vr/DuoViewConfig.hpp",
         "    out.native_fix_same_pass=requested.native_fix_same_pass;",
         "    out.native_fix_same_pass=requested.native_fix_same_pass;\n"
         "    out.quadview_spike_enabled=requested.quadview_spike_enabled;")

    # --- 3. Include the spike header in FFakeStereoRenderingHook.cpp ---
    edit("src/mods/vr/FFakeStereoRenderingHook.cpp",
         '#include "DuoViewGeometry.hpp"',
         '#include "DuoViewGeometry.hpp"\n#include "vr/QuadViewSpike.hpp"')

    # --- 4. Spike active check (static helper) ---
    # Prototype: gated on the setting only (default false). Andrey enables it
    # manually in STALKER 2. Not active when ID6 DuoView is running (no conflict).
    edit("src/mods/vr/FFakeStereoRenderingHook.cpp",
         "void FFakeStereoRenderingHook::adjust_view_rect(FFakeStereoRendering* stereo, int32_t index, int* x, int* y, uint32_t* w, uint32_t* h) {",
         "// QuadView spike: active when the setting is on and ID6 DuoView is\n"
         "// not active (no conflict). Prototype is for STALKER 2 D3D12/OpenXR.\n"
         "static bool quadview_spike_is_active() {\n"
         "    auto& vr = VR::get();\n"
         "    if (!vr) return false;\n"
         "    if (vr->is_duoview_renderer_active()) return false;\n"
         "    return vr->duoview_state().settings().quadview_spike_enabled;\n"
         "}\n"
         "\n"
         "void FFakeStereoRenderingHook::adjust_view_rect(FFakeStereoRendering* stereo, int32_t index, int* x, int* y, uint32_t* w, uint32_t* h) {")

    # --- 5. View count: 4 when spike active ---
    edit("src/mods/vr/FFakeStereoRenderingHook.cpp",
         "    if (vr->is_duoview_renderer_active()) { return 2; }",
         "    if (quadview_spike_is_active()) {\n"
         "        uevr::quadview_spike::counters().frames_requested.fetch_add(1, std::memory_order_relaxed);\n"
         "        return 4;\n"
         "    }\n"
         "    if (vr->is_duoview_renderer_active()) { return 2; }")

    # --- 6. adjust_view_rect: 2x2 grid when spike active ---
    # Insert after the m_skip_next_adjust_view_rect_count block, before the stereo emulation check.
    edit("src/mods/vr/FFakeStereoRenderingHook.cpp",
         "    if (VR::get()->is_stereo_emulation_enabled()) {\n        *w *= 2;",
         "    if (quadview_spike_is_active()) {\n"
         "        // 2x2 grid: quadrants for L-ctx, R-ctx, L-focus, R-focus.\n"
         "        // Normalize index to 0-based (engine may use 0..3 or 1..4).\n"
         "        int32_t qidx = index;\n"
         "        if (qidx >= 1 && qidx <= 4) qidx -= 1;\n"
         "        if (qidx < 0 || qidx > 3) return; // unexpected; leave untouched\n"
         "        const uint32_t full_w = *w;\n"
         "        const uint32_t full_h = *h;\n"
         "        const uint32_t qw = full_w / 2;\n"
         "        const uint32_t qh = full_h / 2;\n"
         "        *x = (qidx % 2) * (int32_t)qw; // 0,2 left; 1,3 right\n"
         "        *y = (qidx / 2) * (int32_t)qh; // 0,1 top; 2,3 bottom\n"
         "        *w = qw;\n"
         "        *h = qh;\n"
         "        uevr::quadview_spike::counters().view_rect_calls[qidx].fetch_add(1, std::memory_order_relaxed);\n"
         "        return;\n"
         "    }\n"
         "    if (VR::get()->is_stereo_emulation_enabled()) {\n        *w *= 2;")

    # --- 7. calculate_stereo_projection_matrix: foveal crop for focus views ---
    # Insert before the DuoView (duo_index) handling, after true_index is computed.
    edit("src/mods/vr/FFakeStereoRenderingHook.cpp",
         "        const bool duo_index=vr->is_duoview_renderer_active() && g_hook->m_has_double_precision &&\n            (view_index == 0 || view_index == 1);",
         "        const bool quad_spike = quadview_spike_is_active();\n"
         "        int32_t quad_idx = view_index;\n"
         "        if (quad_spike && quad_idx >= 1 && quad_idx <= 4) quad_idx -= 1;\n"
         "        const bool quad_focus = quad_spike && (quad_idx == 2 || quad_idx == 3);\n"
         "        if (quad_spike && quad_idx >= 0 && quad_idx <= 3) {\n"
         "            uevr::quadview_spike::counters().projection_calls[quad_idx].fetch_add(1, std::memory_order_relaxed);\n"
         "        }\n"
         "        const bool duo_index=vr->is_duoview_renderer_active() && g_hook->m_has_double_precision &&\n"
         "            (view_index == 0 || view_index == 1);")

    # Apply the crop after the standard projection is set (for focus views).
    # Anchor on the duo_fovea/duo_full block.
    edit("src/mods/vr/FFakeStereoRenderingHook.cpp",
         "        if (duo_fovea || duo_full) {",
         "        if (quad_focus) {\n"
         "            // Foveal crop: 0.5x0.5 center, applied to the standard eye projection.\n"
         "            // true_index already maps 2->0 (left), 3->1 (right) via %2.\n"
         "            // For a centered 0.5x0.5 crop: scale x,y by 2.0.\n"
         "            if (!g_hook->m_has_double_precision) {\n"
         "                (*out)[0][0] *= 2.0f;\n"
         "                (*out)[1][1] *= 2.0f;\n"
         "            } else {\n"
         "                auto& dm = *(Matrix4x4d*)out;\n"
         "                dm[0][0] *= 2.0;\n"
         "                dm[1][1] *= 2.0;\n"
         "            }\n"
         "        }\n"
         "        if (duo_fovea || duo_full) {")

    # --- 8. calculate_stereo_view_offset: don't misdetect 4-view as 1-based ---
    # The stock logic sets index_starts_from_one=true when it sees view_index==2,
    # assuming 1-based 2-view (1,2). For the spike's 4 views (0,1,2,3 or 1,2,3,4),
    # view 2 is the left focus view, not a 1-based right eye. Suppress the
    # misdetection when the spike is active so %2 maps 2->0, 3->1 correctly.
    edit("src/mods/vr/FFakeStereoRenderingHook.cpp",
         "    if (!medium_one_based_eye_pass) {\n"
         "        if (view_index == 2) {\n"
         "            index_starts_from_one = true;\n"
         "            index_was_ever_two = true;",
         "    if (!medium_one_based_eye_pass) {\n"
         "        if (view_index == 2 && !quadview_spike_is_active()) {\n"
         "            index_starts_from_one = true;\n"
         "            index_was_ever_two = true;")

    for path, text in changed.items():
        (source_root(path) / path).write_text(text, encoding="utf-8", newline="")
    print("QuadView spike patch applied (4-view production, 2x2 grid, diagnostics).")

if __name__ == "__main__":
    main()
