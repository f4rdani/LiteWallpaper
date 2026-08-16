#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#if __has_include(<dxgi1_4.h>)
#include <dxgi1_4.h>
#endif
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace litewp {

struct DisplayViewport {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

class D3D11Presenter {
public:
    D3D11Presenter();
    ~D3D11Presenter();

    // Initialize D3D11 device and swap chain on target HWND (gpu_index: -1=CPU/Default, 0=GPU 1, 1=GPU 2, etc.)
    bool Init(HWND hwnd, int width, int height, int gpu_index = 0);
    
    // Render NV12 texture to swap chain with scaling mode and optional target viewports
    void RenderFrame(ID3D11Texture2D* nv12_texture, int array_index, int scaling_mode = 0, const std::vector<DisplayViewport>& target_viewports = {});
    
    // Present frame to screen (syncInterval 0 for software pacing, 1 for monitor VSync)
    HRESULT Present(UINT syncInterval = 0);

    // Query live dedicated/shared video memory used by this process on the active adapter (in MB)
    size_t GetVramUsageMB() const;

    // Diagnostic: clear the swap chain to a solid color and present once.
    void ClearAndPresent(float r, float g, float b);
    
    // Resize swap chain (when monitor resolution changes)
    void Resize(int width, int height);
    
    // Get D3D11 device and context
    ID3D11Device* GetDevice() const;
    ID3D11DeviceContext* GetContext() const;
    
    void Cleanup();

private:
    ComPtr<ID3D11Device>             m_device;
    ComPtr<ID3D11DeviceContext>      m_context;
    ComPtr<IDXGISwapChain>           m_swapchain;
    ComPtr<ID3D11RenderTargetView>   m_rtv;
    
    // NV12 -> RGB conversion resources
    ComPtr<ID3D11PixelShader>        m_nv12_ps;
    ComPtr<ID3D11VertexShader>       m_fullscreen_vs;
    ComPtr<ID3D11SamplerState>       m_sampler;
    ComPtr<ID3D11Buffer>             m_scaling_cb;

    // Shader Resource Views on single NV12 planar texture
    ComPtr<ID3D11ShaderResourceView> m_srv_y;
    ComPtr<ID3D11ShaderResourceView> m_srv_uv;

    // Single-slice NV12 GPU staging texture with BIND_SHADER_RESOURCE
    ComPtr<ID3D11Texture2D>          m_srv_texture;
    UINT                             m_srv_width = 0;
    UINT                             m_srv_height = 0;
    
    HWND m_hwnd = nullptr;
    int m_width = 0;
    int m_height = 0;
    
    bool CreateShaders();
    bool CreateSwapChain(HWND hwnd, int width, int height);
};

} // namespace litewp
