#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#if __has_include(<dxgi1_3.h>)
#include <dxgi1_3.h>
#endif
#if __has_include(<dxgi1_4.h>)
#include <dxgi1_4.h>
#endif
#include <array>
#include <string>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace miniwp {

struct DisplayViewport {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

// Process-wide shared D3D11 device (non-owning raw pointers).
// The daemon creates the one and only device; the settings panel reuses it and
// only creates its own swap chain on top, saving ~2-4 MB of duplicate driver state.
ID3D11Device* GetSharedD3DDevice();
ID3D11DeviceContext* GetSharedD3DDeviceContext();

class D3D11Presenter {
public:
    static constexpr size_t MAX_ZERO_COPY_SLICES = 32;

    D3D11Presenter();
    ~D3D11Presenter();

    // Initialize D3D11 device and swap chain on target HWND (gpu_index: -1=CPU/iGPU, 0=GPU 1, 1=GPU 2, etc.)
    bool Init(HWND hwnd, int width, int height, int gpu_index = 0);

    // Render NV12 texture slice directly (Zero-Copy Texture2DArray SRV) with scaling, multi-monitor viewports, and smooth-loop crossfade
    void RenderFrame(
        ID3D11Texture2D* nv12_texture,
        int array_index,
        int scaling_mode = 0,
        const std::vector<DisplayViewport>& target_viewports = {},
        float blend_alpha = 0.0f,
        int valid_width = 0,
        int valid_height = 0
    );

    // Capture snapshot of the start frame for seamless crossfade looping (allocated only when auto_smooth_loop is active)
    void CaptureStartFrame(ID3D11Texture2D* nv12_texture, int array_index);
    void ResetStartFrame();

    // Release all per-video SRVs and staging/start textures (called on video switch, stop, or deep suspend)
    void ReleaseVideoResources();

    // Present frame to screen (syncInterval 0 for software pacing, 1 for monitor VSync)
    HRESULT Present(UINT syncInterval = 0);

    // Frame latency waitable object (hardware flip queue synchronization)
    HANDLE GetWaitableObject() const noexcept { return m_frame_latency_waitable_object; }

    // Query live dedicated/shared video memory used by this process on the active adapter (in MB)
    size_t GetVramUsageMB() const;

    const std::string& GetAdapterName() const noexcept { return m_adapter_name; }
    const std::string& GetAdapterType() const noexcept { return m_adapter_type; }
    bool IsSoftwareMode() const noexcept { return m_is_software_mode; }

    // Diagnostic: clear the swap chain to a solid color and present once
    void ClearAndPresent(float r, float g, float b);

    // Resize swap chain when virtual screen resolution changes
    void Resize(int width, int height);

    ID3D11Device* GetDevice() const noexcept { return m_device.Get(); }

    void Cleanup();

private:
    struct ScalingData {
        float uv_scale[2];
        float uv_offset[2];
        float uv_max[2];
        float blend_alpha;
        float has_start_frame;
    };

    struct SliceSRVPair {
        ComPtr<ID3D11ShaderResourceView> y;
        ComPtr<ID3D11ShaderResourceView> uv;
    };

    ComPtr<ID3D11Device>             m_device;
    ComPtr<ID3D11DeviceContext>      m_context;
    ComPtr<IDXGISwapChain>           m_swapchain;
    ComPtr<ID3D11RenderTargetView>   m_rtv;
    HANDLE                           m_frame_latency_waitable_object = nullptr;

    // Precompiled NV12 -> RGB shader pipeline
    ComPtr<ID3D11PixelShader>        m_nv12_ps;
    ComPtr<ID3D11VertexShader>       m_fullscreen_vs;
    ComPtr<ID3D11SamplerState>       m_sampler;
    ComPtr<ID3D11Buffer>             m_scaling_cb;
    ScalingData                      m_last_cb_data = {};
    bool                             m_cb_valid = false;

    // Zero-Copy SRV cache bound directly to D3D11VA Texture2DArray slices
    ID3D11Texture2D*                                 m_cached_source_tex = nullptr;
    UINT                                             m_cached_source_w = 0;
    UINT                                             m_cached_source_h = 0;
    bool                                             m_zero_copy_rejected = false;
    std::array<SliceSRVPair, MAX_ZERO_COPY_SLICES>   m_slice_srvs;

    // Fallback 1-slice NV12 texture (allocated ONLY if a driver disallows SRV on decoder output)
    ComPtr<ID3D11Texture2D>          m_fallback_texture;
    ComPtr<ID3D11ShaderResourceView> m_fallback_srv_y;
    ComPtr<ID3D11ShaderResourceView> m_fallback_srv_uv;
    UINT                             m_fallback_width = 0;
    UINT                             m_fallback_height = 0;

    // Start frame snapshot for auto smooth looping crossfade
    ComPtr<ID3D11Texture2D>          m_start_texture;
    ComPtr<ID3D11ShaderResourceView> m_start_srv_y;
    ComPtr<ID3D11ShaderResourceView> m_start_srv_uv;
    UINT                             m_start_width = 0;
    UINT                             m_start_height = 0;

    HWND        m_hwnd = nullptr;
    int         m_width = 0;
    int         m_height = 0;
    std::string m_adapter_name = "Default GPU";
    std::string m_adapter_type = "DirectX 11";
    bool        m_is_software_mode = false;

    bool CreateShaders();
    bool CreateSwapChain(HWND hwnd, int width, int height);
    bool AcquireFrameSRVs(
        ID3D11Texture2D* nv12_texture,
        const D3D11_TEXTURE2D_DESC& texDesc,
        int array_index,
        ID3D11ShaderResourceView*& out_y,
        ID3D11ShaderResourceView*& out_uv
    );
    void CloseWaitableHandle() noexcept;
};

} // namespace miniwp
