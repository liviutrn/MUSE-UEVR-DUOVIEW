# QuadView Prototype Spike

**Status:** Experimental prototype. **Default: inert** (`quadview_spike_enabled=false`).

## What it is

A minimal 4-view production test on top of the 176 DuoView base. When enabled
(via the "QuadView spike" checkbox in the DuoView UI), the engine renders 4 views
per frame instead of 2:

| View | Eye   | Type    | Projection         | Viewport       |
|------|-------|---------|--------------------|----------------|
| 0    | Left  | Context | Full FOV           | Top-left quad  |
| 1    | Right | Context | Full FOV           | Top-right quad |
| 2    | Left  | Focus   | 0.5x0.5 center crop| Bottom-left quad|
| 3    | Right | Focus   | 0.5x0.5 center crop| Bottom-right quad|

The 4 views are rendered in a 2x2 grid. The submit uses the normal path
(whatever it does with 4 views — likely shows the grid or the context pair).
**Diagnostics** (in `uevr::quadview_spike::counters()`) count:
- `frames_requested`: frames where 4 views were requested
- `view_rect_calls[i]`: adjust_view_rect calls per view
- `projection_calls[i]`: projection matrix calls per view

## What it is NOT

- **Not** the full QuadView. No per-eye D3D12 composite (Phase 1).
- **Not** integrated with Joey's native stereo fix (bypasses it for the prototype).
- **Not** gaze-tracked (fixed center crop).

## Purpose

Answer: **can the engine produce 4 views with per-eye poses and foveal crops?**
If the counters show all 4 views rendering with correct projections, the
concept has potential and we proceed to Phase 1 (fix integration + per-eye
composite). If not, we learn why.

## Files

- `QuadViewSpike.hpp`: View constants, diagnostics counters.
- `apply_quadview_spike.py`: Applies the patch to a 176-patched Joey tree.
  Usage: `python3 apply_quadview_spike.py <joey_root> <duoview_src>`

## Patch points

1. `DuoViewConfig.hpp`: Adds `quadview_spike_enabled` setting.
2. `VR.hpp`/`VR.cpp`: UI toggle + Settings wiring.
3. `FFakeStereoRenderingHook.cpp`:
   - `get_desired_number_of_views_hook`: returns 4 when spike active.
   - `adjust_view_rect`: 2x2 grid layout.
   - `calculate_stereo_projection_matrix`: 0.5x0.5 crop for views 2,3.
   - `calculate_stereo_view_offset`: suppresses 1-based misdetection for 4 views.

## Testing

1. Enable "QuadView spike" in the DuoView UI (disable ID6 DuoView first).
2. Run STALKER 2.
3. Check the log for view production (counters).
4. Observe the headset image (expect 2x2 grid or context pair).
5. Report: do all 4 views render? Are the focus crops correct?

## Known limitations

- Submit path not modified; visual may be the 2x2 grid or garbled.
- No shading fix integration; STALKER 2's secondary shading quirk may appear.
- Fixed 0.5x0.5 crop; no configurable density yet.
