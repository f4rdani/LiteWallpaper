#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace litewp {

class D3D11Presenter {
public:
    D3D11Presenter();
    ~D3D11Presenter();

    // Initialize D3D11 device and swap chain on target HWND
    bool Init(HWND hwnd, int width, int height);
    
    // Render NV12 texture to swap chain with scaling mode (0=Fill/Cover, 1=Fit/Letterbox, 2=Stretch)
    void RenderFrame(ID3D11Texture2D* nv12_texture, int array_index, int scaling_mode = 0);
    
    // Present frame to screen (syncInterval 0 for software pacing, 1 for monitor VSync)
    HRESULT Present(UINT syncInterval = 0);

    // Diagnostic: clear the swap chain to a solid color and present once.
    // Used to verify that the render window is actually visible on the desktop.
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
    
    // NV12 → RGB conversion resources
    ComPtr<ID3D11PixelShader>        m_nv12_ps;
    ComPtr<ID3D11VertexShader>       m_fullscreen_vs;
    ComPtr<ID3D11SamplerState>       m_sampler;
    ComPtr<ID3D11Buffer>             m_scaling_cb;

    // GPU Shader Resource Texture & Views for NV12 rendering
    ComPtr<ID3D11Texture2D>          m_srv_texture;
    ComPtr<ID3D11ShaderResourceView> m_srv_y;
    ComPtr<ID3D11ShaderResourceView> m_srv_uv;
    UINT                             m_srv_width = 0;
    UINT                             m_srv_height = 0;
    
    HWND m_hwnd = nullptr;
    int m_width = 0;
    int m_height = 0;
    
    bool CreateShaders();
    bool CreateSwapChain(HWND hwnd, int width, int height);
};

} // namespace litewp
