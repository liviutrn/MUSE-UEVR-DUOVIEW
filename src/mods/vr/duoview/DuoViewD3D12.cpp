// DuoViewD3D12.cpp — D3D12 foveated compositor implementation.
#include "DuoViewD3D12.hpp"
#include "DuoViewGeometry.hpp"

#include <algorithm>
#include <cstring>

#include <spdlog/spdlog.h>

namespace {
// DuoViewComposite.hlsl embedded for D3DCompile at initialize().
// This copy is authoritative; DuoViewComposite.hlsl is the readable reference.
constexpr const char* kDuoViewCompositeHlsl = R"HLSL(
Texture2D<float4> Source : register(t0);
SamplerState LinearClamp : register(s0);
cbuffer DuoViewCompositeConfig : register(b0) { float4 d0; float4 d1; float4 d2; float4 d3; };
float4 VSMain(uint vid : SV_VertexID) : SV_Position {
    float2 p = float2((vid == 1) ? 3.0 : -1.0, (vid == 2) ? 3.0 : -1.0);
    return float4(p, 0.0, 1.0);
}
float4 PSMain(float4 pos : SV_Position) : SV_Target {
    return float4(1.0, 0.0, 0.0, 1.0);
}
)HLSL";
} // namespace

namespace uevr::duoview::dx12 {

void Compositor::shutdown() {
    // ComPtrs release in reverse declaration order; explicit reset for clarity.
    srv_source_.Reset();
    srv_heap_.Reset();
    rtv_heap_.Reset();
    target_.Reset();
    pso_.Reset();
    root_sig_.Reset();
    target_w_ = target_h_ = 0;
}

bool Compositor::initialize(ID3D12Device* device) {
    if (device == nullptr || FAILED(device->GetDeviceRemovedReason())) {
        return false;
    }
    shutdown();

    // Root signature: 16 root constants (b0, pixel) + 1 SRV table (t0, pixel)
    // + static linear-clamp sampler (s0).
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = 16;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MipLODBias = 0.0f;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = 0.0f;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 2;
    rs_desc.pParameters = params;
    rs_desc.NumStaticSamplers = 1;
    rs_desc.pStaticSamplers = &sampler;
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> rs_blob, rs_error;
    if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                          &rs_blob, &rs_error))) {
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(),
                                           rs_blob->GetBufferSize(),
                                           IID_PPV_ARGS(&root_sig_)))) {
        return false;
    }

    // Compile the composite shader once (initialize time, never per-frame).
    // Robust: logs the exact HLSL error if compilation fails, so a bad shader
    // can never silently disable DuoView.
    Microsoft::WRL::ComPtr<ID3DBlob> vs_blob, ps_blob;
    const UINT compile_flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    auto compile = [&](const char* entry, const char* target, ID3DBlob** out) -> bool {
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DCompile(kDuoViewCompositeHlsl, strlen(kDuoViewCompositeHlsl), nullptr,
                                      nullptr, nullptr, entry, target, compile_flags, 0,
                                      out, &errors);
        if (FAILED(hr)) {
            const char* msg = errors ? (const char*)errors->GetBufferPointer() : "(no error blob)";
            spdlog::error("[DuoView] Shader compile failed ({} {}): hr=0x{:08x}: {}",
                          entry, target, (uint32_t)hr, msg);
            return false;
        }
        return true;
    };
    if (!compile("VSMain", "vs_5_0", &vs_blob)) {
        return false;
    }
    if (!compile("PSMain", "ps_5_0", &ps_blob)) {
        return false;
    }

    // PSO: fullscreen-triangle VS (SV_VertexID, no input layout) + composite PS.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root_sig_.Get();
    pso_desc.VS = {vs_blob->GetBufferPointer(), vs_blob->GetBufferSize()};
    pso_desc.PS = {ps_blob->GetBufferPointer(), ps_blob->GetBufferSize()};
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = FALSE;
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    pso_desc.SampleDesc.Count = 1;
    if (FAILED(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso_)))) {
        shutdown();
        return false;
    }

    // RTV heap (1 descriptor, not shader-visible).
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.NumDescriptors = 1;
    if (FAILED(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap_)))) {
        shutdown();
        return false;
    }

    // SRV heap (1 descriptor, shader-visible).
    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc{};
    srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_desc.NumDescriptors = 1;
    srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&srv_heap_)))) {
        shutdown();
        return false;
    }
    srv_descriptor_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

bool Compositor::ensure_target(ID3D12Device* device, uint32_t w, uint32_t h) {
    if (target_ != nullptr && target_w_ == w && target_h_ == h) {
        return true;
    }
    target_.Reset();
    target_w_ = target_h_ = 0;

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET,
                                               &clear, IID_PPV_ARGS(&target_)))) {
        return false;
    }
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
    rtv_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(target_.Get(), &rtv_desc,
                                   rtv_heap_->GetCPUDescriptorHandleForHeapStart());
    target_w_ = w;
    target_h_ = h;
    return true;
}

bool Compositor::ensure_source_srv(ID3D12Device* device, ID3D12Resource* source) {
    if (source == srv_source_.Get() && srv_source_ != nullptr) {
        return true;
    }
    const auto desc = source->GetDesc();
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        return false;
    }
    if (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) {
        return false; // cannot sample as an SRV
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(source, &srv_desc,
                                     srv_heap_->GetCPUDescriptorHandleForHeapStart());
    srv_source_ = source;
    return true;
}

void Compositor::barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res,
                         D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after) {
        return;
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    cmd->ResourceBarrier(1, &b);
}

bool Compositor::composite(ID3D12GraphicsCommandList* cmd, ID3D12Device* device,
                           ID3D12Resource* source, D3D12_RESOURCE_STATES source_state,
                           ID3D12Resource* dest, D3D12_RESOURCE_STATES dest_state,
                           const FrameSnapshot& snapshot,
                           uint32_t slot_w, uint32_t slot_h) {
    if (!initialized() || cmd == nullptr || device == nullptr ||
        source == nullptr || dest == nullptr || source == dest ||
        !snapshot.valid || slot_w == 0 || slot_h == 0) {
        return false;
    }
    // Validate the double-wide layout.
    const auto src_desc = source->GetDesc();
    const auto dst_desc = dest->GetDesc();
    if (src_desc.Width != (uint64_t)slot_w * 2 || src_desc.Height != slot_h ||
        dst_desc.Width != (uint64_t)slot_w * 2 || dst_desc.Height != slot_h ||
        src_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
        dst_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
        src_desc.SampleDesc.Count != 1 || dst_desc.SampleDesc.Count != 1) {
        return false;
    }
    if (!ensure_target(device, slot_w, slot_h)) {
        return false;
    }
    if (!ensure_source_srv(device, source)) {
        return false;
    }

    // Crop rect in full-basis UV. The snapshot stores the fovea center; the
    // size comes from the validated config. A degenerate rect (u0 >= u1)
    // makes the shader output periphery only.
    const auto crop = snapshot.periphery_only
        ? CropRect{0.0f, 0.0f, 0.0f, 0.0f}
        : make_crop_rect(snapshot.fovea_u, snapshot.fovea_v, snapshot.config.crop_fraction);

    // Periphery viewport (in the source's UV space).
    uint32_t pw = slot_w, ph = slot_h;
    snapshot.config.periphery_extent(slot_w, slot_h, pw, ph);
    const float src_w = (float)src_desc.Width;
    const float src_h = (float)src_desc.Height;

    Constants c{};
    c.crop_u0 = crop.u0; c.crop_v0 = crop.v0;
    c.crop_u1 = crop.u1; c.crop_v1 = crop.v1;
    c.output_w = (float)slot_w; c.output_h = (float)slot_h;
    // Fovea slot: left half of the source.
    c.fovea_ox = 0.0f; c.fovea_oy = 0.0f;
    c.fovea_sx = (float)slot_w / src_w; c.fovea_sy = (float)slot_h / src_h;
    // Periphery: right-half origin, pw x ph texels.
    c.peri_ox = (float)slot_w / src_w; c.peri_oy = 0.0f;
    c.peri_sx = (float)pw / src_w; c.peri_sy = (float)ph / src_h;
    c.feather_uv = snapshot.config.feather_px / (float)slot_w;
    c.debug_border = snapshot.config.debug_border ? 1u : 0u;

    // 1. Draw the composite into the internal target.
    barrier(cmd, source, source_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    // (target_ is in RENDER_TARGET from creation/idle.)
    const auto rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT vp{0.0f, 0.0f, (float)slot_w, (float)slot_h, 0.0f, 1.0f};
    cmd->RSSetViewports(1, &vp);
    const D3D12_RECT scissor{0, 0, (LONG)slot_w, (LONG)slot_h};
    cmd->RSSetScissorRects(1, &scissor);
    cmd->SetGraphicsRootSignature(root_sig_.Get());
    cmd->SetPipelineState(pso_.Get());
    cmd->SetGraphicsRoot32BitConstants(0, 16, &c, 0);
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootDescriptorTable(1, srv_heap_->GetGPUDescriptorHandleForHeapStart());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);

    // 2. Copy the mono image to both halves of the destination.
    barrier(cmd, target_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(cmd, dest, dest_state, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = target_.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = dest;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box{0, 0, 0, slot_w, slot_h, 1};
    cmd->CopyTextureRegion(&to, 0, 0, 0, &from, &box);            // left half
    cmd->CopyTextureRegion(&to, slot_w, 0, 0, &from, &box);      // right half

    // 3. Restore states.
    barrier(cmd, target_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barrier(cmd, dest, D3D12_RESOURCE_STATE_COPY_DEST, dest_state);
    barrier(cmd, source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, source_state);
    return true;
}

} // namespace uevr::duoview::dx12
