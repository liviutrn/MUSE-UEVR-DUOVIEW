# DuoView v1 — Design: Native-Stereo Hijack for Fixed-Foveated Mono

**Repo:** `liviutrn/MUSE-UEVR-DUOVIEW` · **Baseline:** `main` @ `96eb4af6` (joeyhodge/UEVR @ `5ac7edc2`)
**Status:** DESIGN — for review before implementation. No code yet.
**Target:** STALKER 2 (UE 5.5, D3D12) · PSVR2 via SteamVR OpenXR · RTX 5070

---

## 1. Goal

Repurpose UEVR's existing **Native Stereo** rendering path to produce **fixed-foveated monoscopic VR**:

- The engine keeps rendering **2 views** (its own view management, pacing, render targets — untouched).
- **Left slot** renders a **high-density foveal crop** of the full mono frustum (narrow off-axis projection, same camera pose).
- **Right slot** renders the **full-FOV periphery at low resolution** (same camera pose).
- A D3D12 composite merges both into **one full-FOV mono image**, submitted to **both eyes**.
- Both slots use the **identical mono camera pose — no IPD**. 6DoF head tracking preserved.

The user selects "Native Stereo" in the UI. There is **no new rendering mode**, no new mode ID, no frontend/injector change. Old true-stereo behavior is intentionally retired on the D3D12+OpenXR path (v1 scope).

## 2. Non-goals (v1)

- No D3D11 / OpenVR hijack: those paths keep original stereo behavior (fail-safe, no silent breakage).
- No `XR_VARJO_quad_views` or any runtime extension: SteamVR does not implement Varjo extensions. Extension-based quad views are a dead end on our runtime (verified by research).
- No runtime HLSL compilation (`D3DCompile` never runs on a live frame — v22 lesson).
- No new view management, frame transactions, or copied-family machinery. The engine's two-view stereo bookkeeping is reused as-is.
- No speculative Unreal offsets, no SDK substitutes. Pinned baseline only.

## 3. The interception points (5 small edits)

All paths below are in the pinned baseline (`96eb4af6`).

### 3.1 Zero the IPD — `FFakeStereoRenderingHook::calculate_stereo_view_offset`
(`src/mods/vr/FFakeStereoRenderingHook.cpp`, ~line 30250)

The mono path already computes `eye_offset` as the L/R average (no IPD) and uses head-space rotation only. When the DuoView hijack is active, apply the **same treatment for both slots**: both views share the identical mono camera pose. 6DoF (standing/head delta) flows through unchanged.

New predicate (one place, reused everywhere):
```cpp
// VR.hpp
bool is_duoview_hijack_active() const {
    return m_rendering_method->value() == RenderingMethod::NATIVE_STEREO
        && g_framework->get_renderer_type() == Framework::RendererType::D3D12
        && /* runtime is OpenXR */;
}
```
Gated on renderer + runtime so D3D11/OpenVR keep true stereo.

### 3.2 Per-slot projections — `FFakeStereoRenderingHook::calculate_stereo_projection_matrix`
(~line 30660)

After the existing overwrite (`*out = get_projection_matrix(eye)`), when hijacked:
- **Slot 0 (left): foveal crop projection** — off-axis perspective covering the foveal ROI (see §4). Honors `m_has_double_precision` (`Matrix4x4f` vs `Matrix4x4d`).
- **Slot 1 (right): full-FOV projection** — the runtime's normal projection, unchanged.

### 3.3 Periphery viewport shrink — `FFakeStereoRenderingHook::adjust_view_rect`
(~line 29920)

- Slot 0 keeps its full half (`W×H`): the narrow crop frustum rasterized across the full slot = high pixel density **for free**.
- Slot 1 (periphery) viewport is shrunk to `periphery_scale` (default **0.25 linear**, configurable 0.15–0.50): the engine renders full-FOV at low res into a sub-rect of the right half. The composite upscales it.

### 3.4 Composite + duplicate submit — `D3D12Component::on_frame`
(~line 5255, adjacent to the existing mono branch)

New branch, evaluated **before** the stereo double-wide / native-array / stalker-pair submits, gated on `is_duoview_hijack_active()`:
1. Run the DuoView composite (see §6): fovea slot + periphery slot → one `W×H` mono image.
2. Duplicate it to both halves of the `DOUBLE_WIDE` OpenXR swapchain, reusing the `copy_scene` region-duplication pattern (pure `CopyTextureRegion`, no shader).
3. **Graceful fallback:** if the composite cannot run (validation failure), duplicate the **periphery slot** (a valid full-FOV image) to both eyes = degraded low-res mono. Never a black frame, never a crash.

Interaction note: the `stalker_exact` / `copy_stalker_pair` path (gated on `is_native_stereo_fix_enabled()`) is bypassed when hijacked. The hijack branch is evaluated first; test config must have the native-stereo-fix experiment off.

### 3.5 Submit layer views — `OpenXR.cpp` xrEndFrame layer construction
(~line 4017, where the mono path already overrides poses)

When hijacked, apply the **same layer treatment as Joey's Mono**: both eyes get the common mono pose, per-eye crop FOVs, `imageRect`s into the duplicated halves. Concretely: extend the existing mono condition to `(is_using_mono() || is_duoview_hijack_active())`. The runtime compositor then distorts the duplicated mono image per eye, exactly as it does for Joey Mono today.

## 4. Foveal projection math

Derived from Joey's **common mono projection** (`uevr::mono::geometry()` — the calibration-safe shared basis; orientation from the left eye, center = mean eye position, symmetric tangents covering both frusta).

Given the full-frustum tangents `(l, r, b, t)` and a foveal ROI defined by center `(cu, cv)` in full-image UV ∈ [0,1] and size fraction `s` (default 0.30):

```
half_w = (r - l) / 2 ;  half_h = (t - b) / 2
cu_tan = lerp(l, r, cu) ;  cv_tan = lerp(b, t, cv)
l' = cu_tan - half_w * s ;  r' = cu_tan + half_w * s
b' = cv_tan - half_h * s ;  t' = cv_tan + half_h * s
```

Build the projection with Joey's **+Z-forward Unreal clip convention** (`[2][3] = +1`, `[3][2] = near`, infinite far) — the convention v22's Stage-5 bugfix established (`p[11] = +1`). Unit-tested on host (no headset needed): project known frustum corners, assert the crop matrix maps the ROI to NDC exactly.

Numeric example (per-eye `2000×2000`, `s = 0.30`, periphery `0.25`):
- Fovea slot: 2000×2000 px over 30% of FOV → effective density ≈ **3.3×** the full-FOV density.
- Periphery slot: 500×500 px over 100% FOV.
- Total view pixels ≈ 4.25M vs 8M stereo baseline ≈ **47% fewer view pixels**. (Pixel budget only — not a frametime promise; Lumen/shadow/UE fixed costs don't scale with pixels.)

## 5. Fovea modes: fixed-center default, stabilized gaze tracking

### 5.1 Fixed-center (default, v1 focus)
Fovea ROI locked at image center. No eye-tracking dependency, no per-frame projection changes, TAA/Lumen temporal history stays perfectly stable. This is the mode to validate first.

### 5.2 Gaze-tracked (the anti-tremble design)
User report: the old gaze implementation **trembled badly — the foveal image itself shook**, not just the gaze point. Root causes: raw noisy gaze fed directly into the projection every frame (invalidating temporal history continuously).

The new gaze path is **stabilize → latch → render**:
1. **Acquire** (once per frame): PSVR2 Toolkit CAPI (`psvr2_toolkit_gaze_status`, 0x148-byte packet). Reject on bad magic/size, missing combined direction, blink, backward or non-finite vector. Hold last valid ray ≤ 50 ms; revoke immediately on invalid sample (v22-validated rules).
2. **Stabilize**: adaptive exponential smoothing on the gaze ray (fast when the eye genuinely saccades, slow during fixation) → **deadband** (ignore movements < ~1% of FOV) → **coarse-grid quantization** of the crop center (snap to 1/64 of FOV). The projection matrix therefore changes only on grid crossings — TAA/Lumen keep stable history between crossings.
3. **Latch**: the crop used by *both* the projection hook (§3.2) and the composite (§6) is an **immutable per-frame snapshot** (Joey's snapshot discipline). Gaze is never read mid-frame.
4. **Project** the stabilized ray through the frozen common mono matrix (+Z convention, §4) to a UV; fall back to center when tracking is unavailable.

Mode toggle (Fixed ↔ Gaze) goes through a **deferred frame-boundary transition** (Joey's `Selection` pattern) — never flips state mid-frame, never hitches.

## 6. Composite shader

- **Fullscreen-triangle raster pixel shader**, BGRA8, single `W×H` target.
- **Precompiled at build time** with `fxc.exe` → embedded DXBC bytecode (v22 lesson: no `D3DCompile` on a live frame, ever).
- **Prewarmed** in `create_swapchains` (per command context), like v22.
- Per output pixel: compute full-FOV UV → if inside fovea ROI, remap through the crop rect and sample the **left half**; else sample the **right half** (upscaled periphery). **Feather** the ROI boundary (smoothstep over a configurable 8–16 px band) — hard edges are visible (Varjo's published guidance).
- Resource discipline: compositor owns its target/descriptors/heaps; explicit state transitions; no aliasing with engine targets; device-removal/reset paths handled.
- **WARP-tested**: pixel-exact expectations (fovea center / periphery / feather ramp), 1,024 duplicated pixel pairs compared across all channels, zero mismatches — the same bar v22's production test set.

## 7. Config & UI

New **"DuoView"** section in the VR page (no mode changes anywhere else):
- Fovea mode: Fixed center (default) / Gaze-tracked
- Crop fraction `s` (0.20–0.50, default 0.30)
- Periphery scale (0.15–0.50, default 0.25)
- Feather width, debug border toggle
- Status line: `Fixed center` · `PSVR2 gaze active` · `Gaze unavailable — center fallback` · `D3D12/OpenXR required`

Persisted as `VR_DuoView_*` config keys. **No injector/frontend changes** (no new mode exists).

## 8. What we reuse from v22 (proven, CI-green Oct 14)

- Exact source pins (backend `5ac7edc2`, SDK `3cffbc82`, frontend `60be4b0f`).
- Build-time shader precompilation; compositor prewarm; current-frame pair validation; graceful fallback discipline; +Z clip convention; CAPI packet validation rules; the 41-test original Joey suite stays green (any regression = stop).
- v22's standalone-tested components (`DuoViewGeometry`, `DuoViewGaze`, `DuoViewCompositeRaster.hlsl`, `DuoViewMonoCopyD3D12`) are **reference material** — ported deliberately, not blindly.

## 9. What we deliberately do NOT do

- No new rendering mode or ID. "Native Stereo" **is** DuoView on D3D12+OpenXR.
- No `XR_VARJO_quad_views` / runtime-extension dependency.
- No D3D11/OpenVR hijack (original stereo preserved there — honest scoping).
- No raw gaze → projection path (stabilize + latch always).
- No new view management / frame transactions / copied families (the engine's stereo bookkeeping is reused).
- No speculative UE offsets; no SDK substitutes; no private-header fabrication.

## 10. CI plan (Windows 2022)

Fresh workflow `duoview-ci.yml` (original workflows were excluded from the baseline push):
1. Checkout pinned baseline + submodules via `UVVR_SOURCE_TOKEN`-style secret (**Liviu must add the Actions secret to this repo** — same token pattern as OpenNR).
2. Build `uevr` (D3D12), auxiliary DLLs, and the unmodified frontend (proves nothing broke).
3. Host-side unit tests: foveal projection math, gaze stabilizer (deadband/grid/freeze), rect mapping, config latch.
4. WARP GPU test: composite pixel-exactness + duplicate-to-both-eyes, debug layer enabled.
5. Package injector + DLLs + `READ_FIRST.txt` → artifact with SHA-256 manifest.
6. Gate: any failure = red. No "green except packaging" hand-waving.

## 11. Validation ladder

1. CI green (compile + unit + WARP + package).
2. Source audit against the 5 interception points (§3).
3. **Controlled headset test** (separate folder; original Joey build kept as rollback): STALKER 2, fixed-center first; compare center/periphery detail, watch frametimes and SteamVR takeovers during movement and scene changes.
4. Gaze-tracked mode only after fixed-center is stable in-headset.
5. Label every claim honestly: **CI-verified** vs **in-headset verified**. Never conflate.

## 12. Known risks (no surprises)

- **Temporal effects**: two UE views with different projections may disagree on TAA/Lumen history and exposure. Fixed-center mitigates (static crop = stable history); gaze mode's grid quantization further mitigates. Real STALKER 2 validation required — this is the #1 unknown.
- **Same-pose/different-projection**: the engine has never been asked to do this in the stereo path; only a headset test proves it renders correctly.
- **Native-stereo-fix experiment** (`is_native_stereo_fix_enabled()`): hijack branch takes precedence; keep the setting off during tests; interaction documented, not yet exercised.
- **Performance**: pixel-budget math is not frametime. Measure on the 5070; promise nothing upfront.
- **Periphery viewport shrink**: engine viewport granularity is untested below 0.15 — default 0.25 is conservative.

---

*Next: implement §3–§7 on branch `duoview/v1`, then §10 CI. This document is updated as the design evolves.*
