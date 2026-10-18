// DuoViewComposite.hlsl — fullscreen-triangle foveated compositor.
//
// Input (Source): the double-wide render target (2W x H).
//   - Left half  [0, 0.5) x [0, 1]: foveal crop, full slot resolution (W x H).
//   - Right half [0.5, 1] x [0, 1]: periphery, low-res viewport at the
//     right-half origin (pw x ph texels).
// Output: one W x H mono image. The caller duplicates it to both halves.
//
// For each output pixel:
//   - fullUv = position in the full-basis image [0,1].
//   - Periphery sample: fullUv mapped into the periphery viewport (full FOV).
//   - Fovea sample (when inside CropUv): fullUv mapped through the crop rect
//     into the fovea slot.
//   - Feathered lerp at the crop border; optional debug border.
//
// Precompiled with fxc at build time (never D3DCompile on a live frame).

Texture2D<float4> Source : register(t0);
SamplerState LinearClamp : register(s0);

cbuffer DuoViewCompositeConfig : register(b0)
{
    float4 CropUv;        // u0, v0, u1, v1 of the foveal ROI in full-basis UV.
                          // Degenerate (u0 >= u1) => periphery-only fallback.
    float2 OutputSize;    // W, H of the mono output (one eye).
    float2 FoveaOrigin;   // top-left of the fovea slot in Source UV.
    float2 FoveaSize;     // size of the fovea slot in Source UV.
    float2 PeriOrigin;    // top-left of the periphery viewport in Source UV.
    float2 PeriSize;      // size of the periphery viewport in Source UV.
    float  FeatherUv;     // feather band width in full-basis UV units.
    uint   DebugBorder;   // non-zero => draw an orange ROI border.
};

// Fullscreen triangle from SV_VertexID (no vertex buffer).
float4 VSMain(uint vid : SV_VertexID) : SV_Position
{
    // (-1,-1), (3,-1), (-1,3) covers the viewport.
    float2 p = float2((vid == 1) ? 3.0 : -1.0, (vid == 2) ? 3.0 : -1.0);
    return float4(p, 0.0, 1.0);
}

// Feathered weight: 1 deep inside the ROI, 0 outside, smoothstep between.
float BlendWeight(float2 fullUv)
{
    if (CropUv.x >= CropUv.z || CropUv.y >= CropUv.w)
        return 0.0; // degenerate crop => periphery only
    if (any(fullUv < CropUv.xy) || any(fullUv > CropUv.zw))
        return 0.0;
    if (FeatherUv <= 0.0)
        return 1.0;
    const float2 cropSize = CropUv.zw - CropUv.xy;
    const float2 local = (fullUv - CropUv.xy) / cropSize;
    const float2 toBorder = min(local, 1.0 - local);
    const float edgeDistUv = min(toBorder.x * cropSize.x, toBorder.y * cropSize.y);
    const float t = saturate(edgeDistUv / FeatherUv);
    return t * t * (3.0 - 2.0 * t);
}

float4 PSMain(float4 pos : SV_Position) : SV_Target
{
    const float2 fullUv = (pos.xy) / OutputSize;

    // Periphery covers the full FOV (just at lower density).
    const float2 periUv = PeriOrigin + fullUv * PeriSize;
    const float4 periColor = Source.Sample(LinearClamp, periUv);

    const float w = BlendWeight(fullUv);
    float4 result = periColor;
    if (w > 0.0)
    {
        const float2 roiLocal = (fullUv - CropUv.xy) / (CropUv.zw - CropUv.xy);
        const float2 foveaUv = FoveaOrigin + roiLocal * FoveaSize;
        const float4 foveaColor = Source.Sample(LinearClamp, foveaUv);
        result = lerp(periColor, foveaColor, w);
    }

    if (DebugBorder != 0 && w > 0.02 && w < 0.35)
        result.rgb = float3(1.0, 0.35, 0.05);

    return result;
}
