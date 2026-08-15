#include "d3d11_presenter.h"
#include <d3dcompiler.h>
#include <iostream>

namespace litewp {

static const char* g_vs_source = R"(
struct VS_OUT {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VS_OUT main(uint vertexID : SV_VertexID) {
    VS_OUT o;
    o.uv = float2((vertexID << 1) & 2, vertexID & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)";

static const char* g_ps_source = R"(
Texture2DArray<float>  texY  : register(t0);
Texture2DArray<float2> texUV : register(t1);
SamplerState           samp  : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float y = texY.Sample(samp, float3(uv, 0.0f));
    float2 uv_val = texUV.Sample(samp, float3(uv, 0.0f));
    
    float u = uv_val.x - 0.5f;
    float v = uv_val.y - 0.5f;
    
    float r = y + 1.5748f * v;
    float g = y - 0.1873f * u - 0.4681f * v;
    float b = y + 1.8556f * u;
    
    return float4(saturate(float3(r, g, b)), 1.0f);
}
)";

D3D11Presenter::D3D11Presenter() = default;

D3D11Presenter::~D3D11Presenter() {
    Cleanup();
}

bool D3D11Presenter::Init(HWND hwnd, int width, int height) {
    m_hwnd = hwnd;
    m_width = width;
    m_height = height;

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
    };

    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL actualFeatureLevel;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        featureLevels,
        _countof(featureLevels),
        D3D11_SDK_VERSION,
        &m_device,
        &actualFeatureLevel,
        &m_context
    );

    if (FAILED(hr)) {
        // Fallback without VIDEO_SUPPORT flag if unsupported
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,
            featureLevels,
            _countof(featureLevels),
            D3D11_SDK_VERSION,
            &m_device,
            &actualFeatureLevel,
            &m_context
        );
        if (FAILED(hr)) return false;
    }

    // Enable multithread protection since FFmpeg decodes on worker threads
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(m_device.As(&mt))) {
        mt->SetMultithreadProtected(TRUE);
    }

    if (!CreateSwapChain(hwnd, width, height)) {
        return false;
    }

    if (!CreateShaders()) {
        return false;
    }

    // Create Sampler State (Bilinear clamp)
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = m_device->CreateSamplerState(&sd, &m_sampler);

    return SUCCEEDED(hr);
}

bool D3D11Presenter::CreateSwapChain(HWND hwnd, int width, int height) {
    if (!m_device) return false;

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(m_device.As(&dxgiDevice))) return false;

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) return false;

    ComPtr<IDXGIFactory> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = width > 0 ? width : 800;
    scd.BufferDesc.Height = height > 0 ? height : 600;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 0;
    scd.BufferDesc.RefreshRate.Denominator = 0;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.SampleDesc.Quality = 0;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; // Windows 7+ compatible

    HRESULT hr = factory->CreateSwapChain(m_device.Get(), &scd, &m_swapchain);
    if (FAILED(hr)) return false;

    // Create render target view from backbuffer
    ComPtr<ID3D11Texture2D> backbuffer;
    hr = m_swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    if (FAILED(hr)) return false;

    hr = m_device->CreateRenderTargetView(backbuffer.Get(), nullptr, &m_rtv);
    return SUCCEEDED(hr);
}

bool D3D11Presenter::CreateShaders() {
    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> errorBlob;

    HRESULT hr = D3DCompile(
        g_vs_source,
        strlen(g_vs_source),
        "fullscreen_quad.hlsl",
        nullptr,
        nullptr,
        "main",
        "vs_4_0",
        0,
        0,
        &vsBlob,
        &errorBlob
    );

    if (FAILED(hr)) {
        return false;
    }

    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_fullscreen_vs);
    if (FAILED(hr)) return false;

    ComPtr<ID3DBlob> psBlob;
    hr = D3DCompile(
        g_ps_source,
        strlen(g_ps_source),
        "nv12_to_rgb.hlsl",
        nullptr,
        nullptr,
        "main",
        "ps_4_0",
        0,
        0,
        &psBlob,
        &errorBlob
    );

    if (FAILED(hr)) {
        return false;
    }

    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_nv12_ps);
    return SUCCEEDED(hr);
}

void D3D11Presenter::RenderFrame(ID3D11Texture2D* nv12_texture, int array_index) {
    if (!m_context || !m_rtv || !nv12_texture) return;

    // Create SRV for Y plane (R8_UNORM) and UV plane (R8G8_UNORM)
    D3D11_TEXTURE2D_DESC texDesc;
    nv12_texture->GetDesc(&texDesc);

    ComPtr<ID3D11ShaderResourceView> srv_y;
    ComPtr<ID3D11ShaderResourceView> srv_uv;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDescY = {};
    srvDescY.Format = DXGI_FORMAT_R8_UNORM;
    srvDescY.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDescY.Texture2DArray.FirstArraySlice = array_index;
    srvDescY.Texture2DArray.ArraySize = 1;
    srvDescY.Texture2DArray.MipLevels = 1;

    if (FAILED(m_device->CreateShaderResourceView(nv12_texture, &srvDescY, &srv_y))) {
        return;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDescUV = {};
    srvDescUV.Format = DXGI_FORMAT_R8G8_UNORM;
    srvDescUV.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDescUV.Texture2DArray.FirstArraySlice = array_index;
    srvDescUV.Texture2DArray.ArraySize = 1;
    srvDescUV.Texture2DArray.MipLevels = 1;

    if (FAILED(m_device->CreateShaderResourceView(nv12_texture, &srvDescUV, &srv_uv))) {
        return;
    }

    // Set Render Target
    m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);

    // Set Viewport
    D3D11_VIEWPORT vp = {0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f};
    m_context->RSSetViewports(1, &vp);

    // Bind Shaders and Textures
    m_context->VSSetShader(m_fullscreen_vs.Get(), nullptr, 0);
    m_context->PSSetShader(m_nv12_ps.Get(), nullptr, 0);

    ID3D11ShaderResourceView* srvs[] = { srv_y.Get(), srv_uv.Get() };
    m_context->PSSetShaderResources(0, 2, srvs);
    m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());

    // Draw Fullscreen Triangle (3 vertices)
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->IASetInputLayout(nullptr);
    m_context->Draw(3, 0);

    // Unbind SRVs
    ID3D11ShaderResourceView* null_srvs[] = { nullptr, nullptr };
    m_context->PSSetShaderResources(0, 2, null_srvs);
}

void D3D11Presenter::Present(UINT syncInterval) {
    if (m_swapchain) {
        m_swapchain->Present(syncInterval, 0);
    }
}

void D3D11Presenter::Resize(int width, int height) {
    if (!m_swapchain || width <= 0 || height <= 0) return;

    m_width = width;
    m_height = height;

    m_rtv.Reset();
    m_context->OMSetRenderTargets(0, nullptr, nullptr);

    m_swapchain->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);

    ComPtr<ID3D11Texture2D> backbuffer;
    if (SUCCEEDED(m_swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
        m_device->CreateRenderTargetView(backbuffer.Get(), nullptr, &m_rtv);
    }
}

ID3D11Device* D3D11Presenter::GetDevice() const {
    return m_device.Get();
}

ID3D11DeviceContext* D3D11Presenter::GetContext() const {
    return m_context.Get();
}

void D3D11Presenter::Cleanup() {
    m_sampler.Reset();
    m_nv12_ps.Reset();
    m_fullscreen_vs.Reset();
    m_rtv.Reset();
    m_swapchain.Reset();
    m_context.Reset();
    m_device.Reset();
}

} // namespace litewp
