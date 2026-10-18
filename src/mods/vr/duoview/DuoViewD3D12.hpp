// DuoViewD3D12.hpp — D3D12 foveated compositor (Windows-only).
//
// Composites the double-wide DuoView source (left half = full-res foveal
// crop, right half = low-res full-FOV periphery) into one W x H mono image,
// then duplicates it to both halves of the destination swapchain texture.
//
// The shader (DuoViewComposite.hlsl) is compiled with D3DCompile once at
// initialize() time (during swapchain creation, never on a live frame).
// The PSO is prewarmed by initialize() so the first composited frame does
// not hitch.
//
// Failure policy: composite() returns false on ANY validation or D3D12
// failure. The caller then falls back to a periphery-only composite
// (degraded mono, never a black frame).
#pragma once

#ifdef _WIN32

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdint>
#include <mutex>

#include "DuoViewConfig.hpp"
#include "DuoViewGeometry.hpp"


namespace uevr::duoview::dx12 {

class Compositor {
public:
    Compositor() = default;
    Compositor(const Compositor&) = delete;
    Compositor& operator=(const Compositor&) = delete;
    ~Compositor() { shutdown(); }

    // Create the PSO, root signature, heaps. Prewarms the PSO.
    // Safe to call more than once (reinitializes).
    bool initialize(ID3D12Device* device);

    void shutdown();

    bool initialized() const { return pso_ != nullptr; }

    // Composite source (2W x H double-wide) into a mono image and duplicate
    // it to both halves of dest (2W x H). source_state/dest_state are the
    // current states; both are restored before returning.
    // snapshot: the frame's validated DuoView state (crop rect etc.).
    // slot_w/slot_h: one eye's dimensions (W x H).
    bool composite(ID3D12GraphicsCommandList* cmd, ID3D12Device* device,
                   ID3D12Resource* source, D3D12_RESOURCE_STATES source_state,
                   ID3D12Resource* dest, D3D12_RESOURCE_STATES dest_state,
                   const FrameSnapshot& snapshot,
                   uint32_t slot_w, uint32_t slot_h);

private:
    // 16 dwords of root constants (must match the HLSL cbuffer layout).
    struct Constants {
        float crop_u0, crop_v0, crop_u1, crop_v1;
        float output_w, output_h;
        float fovea_ox, fovea_oy, fovea_sx, fovea_sy;
        float peri_ox, peri_oy, peri_sx, peri_sy;
        float feather_uv;
        uint32_t debug_border;
    };
    static_assert(sizeof(Constants) == 16 * 4, "root constant size mismatch");

    bool ensure_target(ID3D12Device* device, uint32_t w, uint32_t h);
    bool ensure_source_srv(ID3D12Device* device, ID3D12Resource* source);
    void barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res,
                 D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_sig_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
    // Internal mono target (W x H) + RTV heap.
    Microsoft::WRL::ComPtr<ID3D12Resource> target_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    uint32_t target_w_{}, target_h_{};
    // Shader-visible SRV heap (1 descriptor) for the source.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> srv_source_;
    UINT srv_descriptor_size_{};
};

} // namespace uevr::duoview::dx12

#endif // _WIN32
